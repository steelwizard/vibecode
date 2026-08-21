/*
 * console.c — VGA text mode (0xB8000) + COM1 serial output.
 *
 * Maintains a scrollback buffer so Page Up/Down can review prior output.
 * Full-screen .COM programs run in "direct VGA" mode; the prior shell
 * screen is saved before launch and restored on exit.
 */

#include "console.h"
#include "string.h"
#include "font8x16.h"
#include "keyboard.h"
#include "timer.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_TEXT_COLS  80
#define VGA_TEXT_ROWS  25

/* Upper bound on the text grid: 2560x1440 with the 8x16 font. */
#define CONSOLE_MAX_COLS 320
#define CONSOLE_MAX_ROWS 90
#define SCROLLBACK_MAX 400

#define COM1 0x3F8
#define VGA_CRTC_INDEX 0x3D4
#define VGA_CRTC_DATA  0x3D5

static int term_cols = VGA_TEXT_COLS;
static int term_rows = VGA_TEXT_ROWS;

/*
 * Full-screen programs paint a bar across the bottom row, and the last cell of
 * it used to wrap the cursor and scroll the screen out from under them. Real
 * terminals defer that wrap: the cursor parks in the last column and only moves
 * on when another character actually arrives. Any explicit cursor move or
 * newline cancels it, which is what a redraw does anyway.
 */
static int direct_wrap_pending;
static int fb_active = 0;
static volatile uint32_t *framebuffer = 0;
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;

static const uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static inline uint32_t vga_to_rgb(uint8_t c) {
    return vga_rgb[c & 0x0F];
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t color = 0x0F;
static char *capture_buf = 0;
static size_t capture_cap = 0;
static size_t capture_len = 0;
static int capture_tui;

/* Software block cursor: white glyph on light blue (VGA 9). The hardware
 * cursor is disabled — in framebuffer modes it does not exist, and in text
 * mode it cannot be coloured independently of the cell. */
#define CURSOR_FG 15
#define CURSOR_BG 9

#define CURSOR_BLINK_MS 500

static int cursor_enabled = 1;
static int cursor_painted;
static int cursor_px;
static int cursor_py;
static uint16_t cursor_under;
static uint64_t cursor_blink_at;
static uint16_t screen_cell[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];

#define CLIP_MAX 512
#define HIT_MAX  8
#define HIT_ENTER 1
#define HIT_ESC   2
#define HIT_Y     3
#define HIT_N     4

static int mouse_on;
static int mouse_cx;
static int mouse_cy;
static int mouse_px = -1;
static int mouse_py = -1;
static int spr_saved;
static int spr_defer;
static int spr_sx = -1;
static int spr_sy = -1;
static int sel_on;
static int sel_ax;
static int sel_ay;
static int sel_bx;
static int sel_by;
static char clip[CLIP_MAX];
static int clip_n;

static struct {
    int x, y, w, h, action;
} hits[HIT_MAX];
static int hit_n;

static uint16_t scrollback[SCROLLBACK_MAX][CONSOLE_MAX_COLS];
static int scroll_lines = 1;
static int scroll_view_top = 0;
static int scroll_follow = 1;
static int direct_vga = 0;
static int direct_nest = 0;

typedef struct {
    uint16_t cells[SCROLLBACK_MAX][CONSOLE_MAX_COLS];
    int scroll_lines;
    int scroll_view_top;
    int scroll_follow;
    int cursor_x;
    int cursor_y;
    uint8_t color;
} console_session_t;

static console_session_t saved_session;

static uint16_t blank_cell(void) {
    return (uint16_t)(' ' | ((uint16_t)color << 8));
}

static void fb_draw_cell(int col, int row, uint16_t cell);
static void paint_cell(int x, int y);
static void mouse_spr_restore(void);
static void mouse_spr_redraw(void);

static int spr_cell_hit(int col, int row, int px, int py) {
    int x0;
    int y0;
    int x1;
    int y1;

    if (px < 0 || py < 0) {
        return 0;
    }
    x0 = px / 8;
    y0 = py / 16;
    x1 = (px + 11) / 8;
    y1 = (py + 17) / 16;
    return col >= x0 && col <= x1 && row >= y0 && row <= y1;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_putchar(char c) {
    int spins;

    /* Full-screen .COM UIs draw via VGA; echoing every cell to COM1 can
     * block forever when -serial stdio isn't draining (and it garbles the
     * host terminal with CP437 box chars). */
    if (direct_vga) {
        return;
    }

    for (spins = 0; spins < 100000; spins++) {
        if (serial_ready()) {
            break;
        }
    }
    if (spins >= 100000) {
        return;
    }
    if (c == '\n') {
        outb(COM1, (uint8_t)'\r');
    }
    outb(COM1, (uint8_t)c);
}

static void vga_hide_hw_cursor(void) {
    if (fb_active) {
        return;
    }
    /* CRTC 0x0A bit 5 disables the hardware cursor. */
    outb(VGA_CRTC_INDEX, 0x0A);
    outb(VGA_CRTC_DATA, 0x20);
}

static void hide_soft_cursor(void) {
    if (!cursor_painted) {
        return;
    }
    cursor_painted = 0;
    paint_cell(cursor_px, cursor_py);
}

static void show_soft_cursor(void) {
    uint16_t cell;
    char ch;
    int x;
    int y;

    hide_soft_cursor();
    if (!cursor_enabled || capture_buf) {
        return;
    }
    if (!direct_vga && !scroll_follow) {
        return;
    }
    x = cursor_x;
    y = cursor_y;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= term_cols) {
        x = term_cols - 1;
    }
    if (y >= term_rows) {
        y = term_rows - 1;
    }
    cell = screen_cell[y][x];
    ch = (char)(cell & 0xFF);
    if (ch == 0) {
        ch = ' ';
    }
    cursor_under = (uint16_t)((uint8_t)ch | (cell & 0xFF00));
    cell = (uint16_t)((uint8_t)ch | ((uint16_t)((CURSOR_BG << 4) | CURSOR_FG) << 8));
    mouse_spr_restore();
    if (fb_active) {
        fb_draw_cell(x, y, cell);
    } else {
        VGA_MEM[y * term_cols + x] = cell;
    }
    cursor_px = x;
    cursor_py = y;
    cursor_painted = 1;
    if (mouse_on && x == mouse_cx && y == mouse_cy) {
        paint_cell(x, y);
    }
    mouse_spr_redraw();
}

static void sync_hw_cursor(void) {
    vga_hide_hw_cursor();
    /* Movement / typing: show the block immediately and restart the blink. */
    cursor_blink_at = timer_ticks_ms() + CURSOR_BLINK_MS;
    show_soft_cursor();
}

void console_tick_cursor(void) {
    uint64_t now;

    if (!cursor_enabled || capture_buf) {
        return;
    }
    if (!direct_vga && !scroll_follow) {
        return;
    }
    now = timer_ticks_ms();
    if (now < cursor_blink_at) {
        return;
    }
    cursor_blink_at = now + CURSOR_BLINK_MS;
    if (cursor_painted) {
        hide_soft_cursor();
    } else {
        show_soft_cursor();
    }
    mouse_spr_redraw();
}

static uint16_t invert_attr(uint16_t cell) {
    uint8_t a = (uint8_t)(cell >> 8);
    uint8_t fg = a & 0x0F;
    uint8_t bg = (a >> 4) & 0x07;
    return (uint16_t)((cell & 0xFF) | ((uint16_t)((fg << 4) | bg) << 8));
}

static int cell_in_sel(int x, int y) {
    int x0 = sel_ax;
    int y0 = sel_ay;
    int x1 = sel_bx;
    int y1 = sel_by;

    if (!sel_on) {
        return 0;
    }
    if (y0 > y1 || (y0 == y1 && x0 > x1)) {
        int t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    if (y < y0 || y > y1) {
        return 0;
    }
    if (y0 == y1) {
        return x >= x0 && x <= x1;
    }
    if (y == y0) {
        return x >= x0;
    }
    if (y == y1) {
        return x <= x1;
    }
    return 1;
}

static uint16_t vis_cell(int x, int y) {
    uint16_t cell = screen_cell[y][x];
    if (cell_in_sel(x, y)) {
        cell = invert_attr(cell);
    }
    if (mouse_on && !capture_buf && !fb_active && x == mouse_cx && y == mouse_cy) {
        cell = (uint16_t)(0xDB | ((uint16_t)((4 << 4) | 14) << 8));
    }
    return cell;
}

static void paint_cell(int x, int y) {
    uint16_t cell;
    if (x < 0 || y < 0 || x >= term_cols || y >= term_rows) {
        return;
    }
    cell = vis_cell(x, y);
    if (!spr_defer && fb_active && mouse_on &&
        ((spr_saved && spr_cell_hit(x, y, spr_sx, spr_sy)) ||
         spr_cell_hit(x, y, mouse_px, mouse_py))) {
        mouse_spr_restore();
        if (fb_active) {
            fb_draw_cell(x, y, cell);
        } else {
            VGA_MEM[y * term_cols + x] = cell;
        }
        mouse_spr_redraw();
        return;
    }
    if (fb_active) {
        fb_draw_cell(x, y, cell);
    } else {
        VGA_MEM[y * term_cols + x] = cell;
    }
}

static void clip_add(char c) {
    if (clip_n + 1 < CLIP_MAX) {
        clip[clip_n++] = c;
        clip[clip_n] = 0;
    }
}

static void selection_copy(void) {
    int x0 = sel_ax, y0 = sel_ay, x1 = sel_bx, y1 = sel_by;
    int y;

    clip_n = 0;
    clip[0] = 0;
    if (!sel_on) {
        return;
    }
    if (y0 > y1 || (y0 == y1 && x0 > x1)) {
        int t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    for (y = y0; y <= y1; y++) {
        int xs = (y == y0) ? x0 : 0;
        int xe = (y == y1) ? x1 : term_cols - 1;
        int x;
        int end = xs;
        if (y < 0 || y >= term_rows) {
            continue;
        }
        if (xs < 0) {
            xs = 0;
        }
        if (xe >= term_cols) {
            xe = term_cols - 1;
        }
        for (x = xs; x <= xe; x++) {
            char ch = (char)(screen_cell[y][x] & 0xFF);
            if (ch != ' ' && ch != 0) {
                end = x + 1;
            }
        }
        for (x = xs; x < end; x++) {
            char ch = (char)(screen_cell[y][x] & 0xFF);
            if (ch == 0) {
                ch = ' ';
            }
            clip_add(ch);
        }
        if (y != y1) {
            clip_add('\n');
        }
    }
}

static void stamp_cell(int x, int y, uint16_t cell) {
    if (x < 0 || y < 0 || x >= term_cols || y >= term_rows) {
        return;
    }
    hide_soft_cursor();
    screen_cell[y][x] = cell;
    paint_cell(x, y);
}

static void scroll_drop_oldest(void) {
    int i;

    if (scroll_lines <= 1) {
        return;
    }
    for (i = 1; i < scroll_lines; i++) {
        memcpy(scrollback[i - 1], scrollback[i],
               (size_t)term_cols * sizeof(uint16_t));
    }
    scroll_lines--;
    if (scroll_view_top > 0) {
        scroll_view_top--;
    }
}

/* Map a growing absolute line number onto the ring of SCROLLBACK_MAX rows. */
static int scroll_ensure_line(int abs_line) {
    if (abs_line < 0) {
        return 0;
    }
    while (abs_line >= SCROLLBACK_MAX) {
        scroll_drop_oldest();
        abs_line--;
        if (abs_line < 0) {
            return 0;
        }
    }
    while (scroll_lines <= abs_line) {
        int x;
        for (x = 0; x < term_cols; x++) {
            scrollback[scroll_lines][x] = blank_cell();
        }
        scroll_lines++;
    }
    if (abs_line >= scroll_lines) {
        abs_line = scroll_lines - 1;
    }
    return abs_line;
}

static void scroll_pin_bottom(void) {
    scroll_follow = 1;
    scroll_view_top = scroll_lines > term_rows ? scroll_lines - term_rows : 0;
}

static void fb_draw_cell(int col, int row, uint16_t cell) {
    char ch = (char)(cell & 0xFF);
    uint8_t attr = (uint8_t)(cell >> 8);
    uint8_t fg = attr & 0x0F;
    uint8_t bg = (attr >> 4) & 0x0F;
    const uint8_t *glyph = &font8x16[(unsigned char)ch * 16];
    int px = col * 8;
    int py = row * 16;

    if (!framebuffer || px + 8 > (int)fb_width || py + 16 > (int)fb_height) {
        return;
    }

    for (int dy = 0; dy < 16; dy++) {
        volatile uint32_t *line = (volatile uint32_t *)((uintptr_t)framebuffer +
                                                        (uint32_t)(py + dy) * fb_pitch);
        uint8_t bits = glyph[dy];
        for (int dx = 0; dx < 8; dx++) {
            line[px + dx] = (bits & (0x80 >> dx)) ? vga_to_rgb(fg) : vga_to_rgb(bg);
        }
    }
}

static void fb_clear_screen(uint8_t bg) {
    uint32_t rgb = vga_to_rgb(bg);

    if (!framebuffer) {
        return;
    }

    for (uint32_t y = 0; y < fb_height; y++) {
        volatile uint32_t *line = (volatile uint32_t *)((uintptr_t)framebuffer + y * fb_pitch);
        uint32_t x = 0;

        while (x + 4 <= fb_width) {
            line[x] = rgb;
            line[x + 1] = rgb;
            line[x + 2] = rgb;
            line[x + 3] = rgb;
            x += 4;
        }
        while (x < fb_width) {
            line[x++] = rgb;
        }
    }
}

#define SPR_W 12
#define SPR_H 18
#define SPR_FILL 0xFFFF55u
#define SPR_EDGE 0x000000u

/* 0 = skip, 1 = black outline, 2 = yellow fill */
static const uint8_t spr_map[SPR_H][SPR_W] = {
    {1,2,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,0,0,0},
    {1,2,2,2,2,2,2,2,2,2,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0},
};

static uint32_t spr_under[SPR_H][SPR_W];

static void mouse_spr_restore(void) {
    int dy;
    int dx;

    if (!spr_saved || !framebuffer) {
        spr_saved = 0;
        return;
    }
    for (dy = 0; dy < SPR_H; dy++) {
        int y = spr_sy + dy;
        if (y < 0 || y >= (int)fb_height) {
            continue;
        }
        volatile uint32_t *line = (volatile uint32_t *)((uintptr_t)framebuffer +
                                                        (uint32_t)y * fb_pitch);
        for (dx = 0; dx < SPR_W; dx++) {
            int x = spr_sx + dx;
            if (x >= 0 && x < (int)fb_width) {
                line[x] = spr_under[dy][dx];
            }
        }
    }
    spr_saved = 0;
}

static void mouse_spr_redraw(void) {
    int dy;
    int dx;
    int x0;
    int y0;

    if (spr_defer || !fb_active || !framebuffer || !mouse_on || capture_buf ||
        mouse_px < 0) {
        return;
    }
    x0 = mouse_px;
    y0 = mouse_py;
    mouse_spr_restore();
    for (dy = 0; dy < SPR_H; dy++) {
        int y = y0 + dy;
        if (y < 0 || y >= (int)fb_height) {
            continue;
        }
        volatile uint32_t *line = (volatile uint32_t *)((uintptr_t)framebuffer +
                                                        (uint32_t)y * fb_pitch);
        for (dx = 0; dx < SPR_W; dx++) {
            int x = x0 + dx;
            uint8_t p;
            if (x < 0 || x >= (int)fb_width) {
                continue;
            }
            spr_under[dy][dx] = line[x];
            p = spr_map[dy][dx];
            if (p == 2) {
                line[x] = SPR_FILL;
            } else if (p == 1) {
                line[x] = SPR_EDGE;
            }
        }
    }
    spr_sx = x0;
    spr_sy = y0;
    spr_saved = 1;
}

static void console_redraw(void) {
    cursor_painted = 0;
    spr_saved = 0;
    spr_defer = 1;
    for (int y = 0; y < term_rows; y++) {
        int sl = scroll_view_top + y;
        for (int x = 0; x < term_cols; x++) {
            uint16_t cell = blank_cell();
            if (sl >= 0 && sl < scroll_lines) {
                cell = scrollback[sl][x];
            }
            screen_cell[y][x] = cell;
            paint_cell(x, y);
        }
    }
    spr_defer = 0;
    sync_hw_cursor();
    mouse_spr_redraw();
}

static void scroll_write_cell(int abs_line, int x, uint16_t cell) {
    abs_line = scroll_ensure_line(abs_line);
    if (x >= 0 && x < CONSOLE_MAX_COLS && abs_line >= 0 &&
        abs_line < SCROLLBACK_MAX) {
        scrollback[abs_line][x] = cell;
    }
    if (!direct_vga) {
        int vy = abs_line - scroll_view_top;
        if (vy >= 0 && vy < term_rows && x >= 0 && x < term_cols) {
            stamp_cell(x, vy, cell);
        }
    }
}

static void scroll_newline(void) {
    int next_abs;

    cursor_x = 0;
    direct_wrap_pending = 0;
    next_abs = scroll_ensure_line(scroll_view_top + cursor_y + 1);

    if (next_abs >= scroll_view_top + term_rows) {
        cursor_y = term_rows - 1;
        if (scroll_follow) {
            scroll_pin_bottom();
        }
        console_redraw();
    } else {
        cursor_y++;
    }
    sync_hw_cursor();
}

static void direct_scroll(void) {
    uint16_t blank;

    if (cursor_y < term_rows) {
        return;
    }
    hide_soft_cursor();
    mouse_spr_restore();
    for (int y = 0; y < term_rows - 1; y++) {
        memcpy(screen_cell[y], screen_cell[y + 1], (size_t)term_cols * sizeof(uint16_t));
    }
    blank = blank_cell();
    for (int x = 0; x < term_cols; x++) {
        screen_cell[term_rows - 1][x] = blank;
    }
    if (fb_active && framebuffer) {
        uint8_t *fb = (uint8_t *)(uintptr_t)framebuffer;
        uint32_t block = (uint32_t)(term_rows - 1) * 16U * fb_pitch;
        for (uint32_t i = 0; i < block; i++) {
            fb[i] = fb[i + 16U * fb_pitch];
        }
        for (int x = 0; x < term_cols; x++) {
            fb_draw_cell(x, term_rows - 1, blank);
        }
    } else {
        for (int y = 1; y < term_rows; y++) {
            for (int x = 0; x < term_cols; x++) {
                VGA_MEM[(y - 1) * term_cols + x] = VGA_MEM[y * term_cols + x];
            }
        }
        for (int x = 0; x < term_cols; x++) {
            VGA_MEM[(term_rows - 1) * term_cols + x] = blank;
        }
    }
    cursor_y = term_rows - 1;
    sync_hw_cursor();
    mouse_spr_redraw();
}

static void putchar_at(char c, int x, int y) {
    stamp_cell(x, y, (uint16_t)((uint8_t)c | ((uint16_t)color << 8)));
}

static void direct_emit(char c) {
    if (direct_wrap_pending) {
        direct_wrap_pending = 0;
        cursor_x = 0;
        cursor_y++;
        direct_scroll();
    }
    putchar_at(c, cursor_x, cursor_y);
    if (cursor_x + 1 >= term_cols) {
        direct_wrap_pending = 1;
    } else {
        cursor_x++;
    }
    sync_hw_cursor();
}

void console_init(void) {
    int i;

    serial_init();
    fb_active = 0;
    framebuffer = 0;
    term_cols = VGA_TEXT_COLS;
    term_rows = VGA_TEXT_ROWS;
    color = 0x0F;
    cursor_enabled = 1;
    cursor_painted = 0;
    cursor_x = 0;
    cursor_y = 0;
    scroll_lines = 1;
    scroll_view_top = 0;
    scroll_follow = 1;
    direct_vga = 0;
    direct_wrap_pending = 0;
    for (i = 0; i < term_cols; i++) {
        scrollback[0][i] = blank_cell();
    }
    console_redraw();
}

void console_init_framebuffer(const video_mode_t *mode) {
    if (!mode || mode->fb_addr == 0) {
        return;
    }

    fb_active = 1;
    framebuffer = (volatile uint32_t *)(uintptr_t)mode->fb_addr;
    fb_pitch = mode->pitch;
    fb_width = mode->width;
    fb_height = mode->height;
    term_cols = (int)(mode->width / 8);
    term_rows = (int)(mode->height / 16);
    if (term_cols > CONSOLE_MAX_COLS) {
        term_cols = CONSOLE_MAX_COLS;
    }
    if (term_rows > CONSOLE_MAX_ROWS) {
        term_rows = CONSOLE_MAX_ROWS;
    }

    scroll_lines = 1;
    scroll_view_top = 0;
    scroll_follow = 1;
    direct_vga = 0;
    direct_wrap_pending = 0;
    cursor_x = 0;
    cursor_y = 0;

    for (int i = 0; i < term_cols; i++) {
        scrollback[0][i] = blank_cell();
    }
    fb_clear_screen((color >> 4) & 0x0F);
    spr_saved = 0;
    console_redraw();
}

int console_is_framebuffer(void) {
    return fb_active;
}

void console_get_size(int *cols, int *rows) {
    if (cols) {
        *cols = term_cols;
    }
    if (rows) {
        *rows = term_rows;
    }
}

void console_get_cursor(int *x, int *y) {
    if (x) {
        *x = cursor_x;
    }
    if (y) {
        *y = cursor_y;
    }
}

void console_hit_clear(void) {
    hit_n = 0;
}

void console_hit_add(int x, int y, int w, int h, int action) {
    if (hit_n >= HIT_MAX || w < 1 || h < 1) {
        return;
    }
    hits[hit_n].x = x;
    hits[hit_n].y = y;
    hits[hit_n].w = w;
    hits[hit_n].h = h;
    hits[hit_n].action = action;
    hit_n++;
}

int console_hit_click(int x, int y) {
    int i;
    for (i = 0; i < hit_n; i++) {
        if (x >= hits[i].x && x < hits[i].x + hits[i].w &&
            y >= hits[i].y && y < hits[i].y + hits[i].h) {
            if (hits[i].action == HIT_ENTER) {
                keyboard_inject(KEY_ENTER, 0);
            } else if (hits[i].action == HIT_ESC) {
                keyboard_inject(KEY_CHAR, 27);
            } else if (hits[i].action == HIT_Y) {
                keyboard_inject(KEY_CHAR, 'y');
            } else if (hits[i].action == HIT_N) {
                keyboard_inject(KEY_CHAR, 'n');
            } else {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

void console_mouse_move(int x, int y) {
    int ox = mouse_cx;
    int oy = mouse_cy;

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= term_cols) {
        x = term_cols - 1;
    }
    if (y >= term_rows) {
        y = term_rows - 1;
    }
    mouse_cx = x;
    mouse_cy = y;
    mouse_on = 1;
    if (capture_buf) {
        return;
    }
    if (ox != x || oy != y) {
        paint_cell(ox, oy);
        if (cursor_painted && ox == cursor_px && oy == cursor_py) {
            show_soft_cursor();
        }
    }
    paint_cell(x, y);
    mouse_spr_redraw();
}

void console_mouse_pixel(int px, int py) {
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    mouse_spr_restore();
    mouse_px = px;
    mouse_py = py;
    console_mouse_move(px / 8, py / 16);
}

void console_mouse_left(int phase, int x, int y) {
    static int cover0;
    static int cover1 = -1;
    int y0;
    int y1;
    int a;
    int b;
    int row;
    int col;

    if (capture_buf || direct_vga) {
        return;
    }
    if (phase == 1) {
        if (sel_on) {
            sel_on = 0;
            if (cover1 >= 0) {
                for (row = cover0; row <= cover1; row++) {
                    for (col = 0; col < term_cols; col++) {
                        paint_cell(col, row);
                    }
                }
            }
            cover1 = -1;
        }
        sel_ax = x;
        sel_ay = y;
        sel_bx = x;
        sel_by = y;
        return;
    }
    if (phase != 2) {
        return;
    }
    sel_bx = x;
    sel_by = y;
    if (x == sel_ax && y == sel_ay) {
        return;
    }
    sel_on = 1;
    y0 = sel_ay < sel_by ? sel_ay : sel_by;
    y1 = sel_ay < sel_by ? sel_by : sel_ay;
    a = y0;
    b = y1;
    if (cover1 >= 0) {
        if (cover0 < a) {
            a = cover0;
        }
        if (cover1 > b) {
            b = cover1;
        }
    }
    if (a < 0) {
        a = 0;
    }
    if (b >= term_rows) {
        b = term_rows - 1;
    }
    for (row = a; row <= b; row++) {
        for (col = 0; col < term_cols; col++) {
            paint_cell(col, row);
        }
    }
    cover0 = y0;
    cover1 = y1;
    paint_cell(mouse_cx, mouse_cy);
}

void console_mouse_right(int x, int y) {
    (void)x;
    (void)y;
    if (direct_vga) {
        return;
    }
    if (sel_on) {
        selection_copy();
        return;
    }
    if (clip_n > 0) {
        keyboard_inject_str(clip);
    }
}

void console_clear_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
    console_clear();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
}

void console_set_theme(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
    console_clear();
}

void console_clear(void) {
    int i;

    if (capture_buf) {
        capture_tui = 1;
        capture_buf = 0;
        capture_cap = 0;
    }

    direct_wrap_pending = 0;

    if (direct_vga) {
        uint16_t blank = blank_cell();
        hide_soft_cursor();
        for (int y = 0; y < term_rows; y++) {
            for (int x = 0; x < term_cols; x++) {
                screen_cell[y][x] = blank;
            }
        }
        if (fb_active) {
            fb_clear_screen((color >> 4) & 0x0F);
            spr_saved = 0;
        } else {
            for (i = 0; i < term_cols * term_rows; i++) {
                VGA_MEM[i] = blank;
            }
        }
        cursor_x = 0;
        cursor_y = 0;
        sync_hw_cursor();
        mouse_spr_redraw();
        return;
    }

    scroll_lines = 1;
    scroll_view_top = 0;
    scroll_follow = 1;
    for (i = 0; i < term_cols; i++) {
        scrollback[0][i] = blank_cell();
    }
    cursor_x = 0;
    cursor_y = 0;
    console_redraw();
}

void console_begin_direct(void) {
    if (direct_nest == 0) {
        saved_session.scroll_lines = scroll_lines;
        saved_session.scroll_view_top = scroll_view_top;
        saved_session.scroll_follow = scroll_follow;
        saved_session.cursor_x = cursor_x;
        saved_session.cursor_y = cursor_y;
        saved_session.color = color;
        memcpy(saved_session.cells, scrollback, sizeof(scrollback));
    }
    direct_nest++;
    direct_vga = 1;
    direct_wrap_pending = 0;
    if (direct_nest == 1) {
        cursor_enabled = 0;
        hide_soft_cursor();
    }
}

void console_end_direct(void) {
    if (direct_nest <= 0) {
        return;
    }
    direct_nest--;
    direct_wrap_pending = 0;
    if (direct_nest == 0) {
        direct_vga = 0;
        scroll_lines = saved_session.scroll_lines;
        scroll_view_top = saved_session.scroll_view_top;
        scroll_follow = saved_session.scroll_follow;
        cursor_x = saved_session.cursor_x;
        cursor_y = saved_session.cursor_y;
        color = saved_session.color;
        memcpy(scrollback, saved_session.cells, sizeof(scrollback));
        cursor_enabled = 1;
        cursor_painted = 0;
        console_redraw();
    } else {
        direct_vga = 1;
    }
}

void console_scroll_to_bottom(void) {
    if (direct_vga) {
        return;
    }
    scroll_pin_bottom();
    console_redraw();
}

int console_at_bottom(void) {
    return direct_vga || scroll_follow;
}

void console_page_up(void) {
    int max_top;

    if (direct_vga) {
        return;
    }
    scroll_follow = 0;
    scroll_view_top -= term_rows;
    if (scroll_view_top < 0) {
        scroll_view_top = 0;
    }
    max_top = scroll_lines > term_rows ? scroll_lines - term_rows : 0;
    if (scroll_view_top > max_top) {
        scroll_view_top = max_top;
    }
    console_redraw();
}

void console_page_down(void) {
    int max_top;

    if (direct_vga) {
        return;
    }
    scroll_view_top += term_rows;
    max_top = scroll_lines > term_rows ? scroll_lines - term_rows : 0;
    if (scroll_view_top >= max_top) {
        scroll_view_top = max_top;
        scroll_follow = 1;
    }
    console_redraw();
}

void console_backspace(void) {
    direct_wrap_pending = 0;
    if (cursor_x > 0) {
        cursor_x--;
        if (direct_vga) {
            putchar_at(' ', cursor_x, cursor_y);
        } else {
            scroll_write_cell(scroll_view_top + cursor_y, cursor_x, blank_cell());
        }
        serial_putchar('\b');
        serial_putchar(' ');
        serial_putchar('\b');
        sync_hw_cursor();
    }
}

void console_cursor_back(void) {
    direct_wrap_pending = 0;
    if (cursor_x > 0) {
        cursor_x--;
        sync_hw_cursor();
        serial_putchar('\b');
    } else if (cursor_y > 0) {
        cursor_y--;
        cursor_x = term_cols - 1;
        sync_hw_cursor();
        serial_putchar('\b');
    }
}

void console_goto_xy(int x, int y) {
    if (capture_buf) {
        capture_tui = 1;
        capture_buf = 0;
        capture_cap = 0;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= term_cols) {
        x = term_cols - 1;
    }
    if (y >= term_rows) {
        y = term_rows - 1;
    }
    cursor_x = x;
    cursor_y = y;
    direct_wrap_pending = 0;
    sync_hw_cursor();
}

void console_set_cursor_visible(int visible) {
    cursor_enabled = visible ? 1 : 0;
    if (cursor_enabled) {
        sync_hw_cursor();
    } else {
        hide_soft_cursor();
    }
}

void console_begin_capture(char *buf, size_t cap) {
    capture_tui = 0;
    capture_buf = buf;
    capture_cap = cap;
    capture_len = 0;
    if (cap > 0 && buf) {
        buf[0] = 0;
    }
}

size_t console_end_capture(void) {
    size_t n = capture_len;

    if (capture_tui) {
        capture_tui = 0;
        capture_buf = 0;
        capture_cap = 0;
        capture_len = 0;
        return 0;
    }
    if (capture_buf && capture_cap > 0) {
        if (capture_len >= capture_cap) {
            capture_len = capture_cap - 1;
        }
        capture_buf[capture_len] = 0;
        n = capture_len;
    }
    capture_buf = 0;
    capture_cap = 0;
    return n;
}

int console_is_capturing(void) {
    return capture_buf != 0;
}

static void capture_putchar(char c) {
    if (!capture_buf || capture_cap == 0) {
        return;
    }
    serial_putchar(c);
    if (capture_len + 1 < capture_cap) {
        capture_buf[capture_len++] = c;
        capture_buf[capture_len] = 0;
    }
}

void console_write_n(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        console_putchar(s[i]);
    }
}

void console_putchar(char c) {
    uint16_t cell;

    if (capture_buf) {
        if (c == '\b') {
            if (capture_len > 0) {
                capture_len--;
                capture_buf[capture_len] = 0;
            }
            return;
        }
        capture_putchar(c);
        return;
    }

    if (c == '\b') {
        console_backspace();
        return;
    }

    serial_putchar(c);

    if (!direct_vga && !scroll_follow) {
        console_scroll_to_bottom();
    }

    if (c == '\n') {
        scroll_newline();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        direct_wrap_pending = 0;
        sync_hw_cursor();
        return;
    }

    cell = (uint16_t)((uint8_t)c | ((uint16_t)color << 8));

    if (direct_vga) {
        direct_emit(c);
        return;
    }

    scroll_write_cell(scroll_view_top + cursor_y, cursor_x, cell);
    cursor_x++;
    if (cursor_x >= term_cols) {
        cursor_x = 0;
        if (cursor_y + 1 >= term_rows) {
            scroll_newline();
        } else {
            cursor_y++;
            scroll_ensure_line(scroll_view_top + cursor_y);
            sync_hw_cursor();
        }
    } else {
        sync_hw_cursor();
    }
}

static void emit_char_colored(char c, uint8_t fg, uint8_t bg) {
    uint8_t prev = color;
    uint16_t cell;

    if (capture_buf) {
        capture_putchar(c);
        return;
    }

    serial_putchar(c);
    color = (bg << 4) | (fg & 0x0F);

    if (!direct_vga && !scroll_follow) {
        console_scroll_to_bottom();
    }

    if (c == '\b') {
        color = prev;
        console_backspace();
        return;
    }

    if (c == '\n') {
        color = prev;
        scroll_newline();
        return;
    }

    if (c == '\r') {
        color = prev;
        cursor_x = 0;
        direct_wrap_pending = 0;
        sync_hw_cursor();
        return;
    }

    cell = (uint16_t)((uint8_t)c | ((uint16_t)(((bg & 0x0F) << 4) | (fg & 0x0F)) << 8));

    if (direct_vga) {
        direct_emit((char)(cell & 0xFF));
        color = prev;
        return;
    }

    scroll_write_cell(scroll_view_top + cursor_y, cursor_x, cell);
    cursor_x++;
    if (cursor_x >= term_cols) {
        cursor_x = 0;
        if (cursor_y + 1 >= term_rows) {
            color = prev;
            scroll_newline();
            return;
        }
        cursor_y++;
        scroll_ensure_line(scroll_view_top + cursor_y);
    }
    color = prev;
    sync_hw_cursor();
}

void console_write_color(uint8_t fg, uint8_t bg, const char *s) {
    while (*s) {
        emit_char_colored(*s++, fg, bg);
    }
}

void console_write_line_color(uint8_t fg, uint8_t bg, const char *s) {
    console_write_color(fg, bg, s);
    emit_char_colored('\n', fg, bg);
}

void boot_line(const char *s) {
    console_write_line_color(15, 1, s);
}

void console_write(const char *s) {
    while (*s) {
        console_putchar(*s++);
    }
}

void console_write_line(const char *s) {
    console_write(s);
    console_putchar('\n');
}

void console_write_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(value >> (60 - i * 4)) & 0xF];
    }
    buf[18] = 0;
    console_write(buf);
}

void console_write_dec(uint64_t value) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    if (value == 0) {
        console_putchar('0');
        return;
    }
    while (value > 0 && i > 0) {
        buf[--i] = (char)('0' + (value % 10));
        value /= 10;
    }
    console_write(buf + i);
}

void console_write_size(uint64_t bytes) {
    const char *unit = "B";
    uint64_t whole = bytes;
    uint64_t frac = 0;

    if (bytes >= 1024ULL * 1024 * 1024) {
        unit = "GiB";
        whole = bytes / (1024ULL * 1024 * 1024);
        frac = (bytes % (1024ULL * 1024 * 1024)) * 10 / (1024ULL * 1024 * 1024);
    } else if (bytes >= 1024ULL * 1024) {
        unit = "MiB";
        whole = bytes / (1024ULL * 1024);
        frac = (bytes % (1024ULL * 1024)) * 10 / (1024ULL * 1024);
    } else if (bytes >= 1024ULL) {
        unit = "KiB";
        whole = bytes / 1024ULL;
        frac = (bytes % 1024ULL) * 10 / 1024ULL;
    }

    console_write_dec(whole);
    if (frac > 0 && whole < 100) {
        console_putchar('.');
        console_putchar((char)('0' + (char)frac));
    }
    console_putchar(' ');
    console_write(unit);
}

#define ERR_SCR_FG   15
#define ERR_SCR_BG    4  /* red */
#define ERR_BOX_FG   15
#define ERR_BOX_BG    9  /* bright blue */
#define ERR_EDGE_FG  11  /* cyan */
#define ERR_BOMB_FG  14  /* yellow bomb in the title */
#define ERR_BTN_FG    0  /* black on white — selected TUI button */
#define ERR_BTN_BG    7
#define ERR_SHAD_FG   8
#define ERR_SHAD_BG   4
#define ERR_CH_BOMB  0x0Fu

#define ERR_CH_TL    0xC9u
#define ERR_CH_TR    0xBBu
#define ERR_CH_BL    0xC8u
#define ERR_CH_BR    0xBCu
#define ERR_CH_H     0xCDu
#define ERR_CH_V     0xBAu

#define ERR_MAX_LINES 8
#define ERR_LINE_MAX  56

static void err_cell(int x, int y, unsigned char c, uint8_t fg, uint8_t bg) {
    uint8_t saved = color;
    if (x < 0 || y < 0 || x >= term_cols || y >= term_rows) {
        return;
    }
    color = (uint8_t)((bg << 4) | (fg & 0x0F));
    putchar_at((char)c, x, y);
    color = saved;
}

static void err_fill(int x0, int y0, int x1, int y1, unsigned char c,
                     uint8_t fg, uint8_t bg) {
    int x, y;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            err_cell(x, y, c, fg, bg);
        }
    }
}

static void err_puts(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    while (*s) {
        err_cell(x++, y, (unsigned char)*s++, fg, bg);
    }
}

static int err_wrap(const char *msg, char lines[][ERR_LINE_MAX], int width) {
    int nlines = 0;
    int i = 0;

    if (!msg) {
        msg = "";
    }
    while (msg[i] && nlines < ERR_MAX_LINES) {
        int start;
        int last_sp;
        int end;
        int len;
        int k;

        while (msg[i] == ' ') {
            i++;
        }
        if (!msg[i]) {
            break;
        }
        start = i;
        last_sp = -1;
        while (msg[i] && msg[i] != '\n' && (i - start) < width) {
            if (msg[i] == ' ') {
                last_sp = i;
            }
            i++;
        }
        if (msg[i] == '\n') {
            end = i;
            i++;
        } else if (msg[i] && last_sp > start) {
            end = last_sp;
            i = last_sp + 1;
        } else {
            end = i;
        }
        len = end - start;
        if (len >= width) {
            len = width - 1;
        }
        if (len < 0) {
            len = 0;
        }
        for (k = 0; k < len; k++) {
            lines[nlines][k] = msg[start + k];
        }
        lines[nlines][len] = 0;
        nlines++;
    }
    if (nlines == 0) {
        lines[0][0] = 0;
        nlines = 1;
    }
    return nlines;
}

static void err_wait_ok(void) {
    /* Drop anything already queued so a leftover Enter doesn't skip the dialog. */
    while (keyboard_has_key()) {
        (void)keyboard_read_event();
    }
    for (;;) {
        key_event_t ev;
        while (!keyboard_has_key()) {
            __asm__ volatile("pause");
        }
        ev = keyboard_read_event();
        if (ev.type == KEY_ENTER) {
            return;
        }
        if (ev.type == KEY_CHAR &&
            (ev.ch == ' ' || ev.ch == 'o' || ev.ch == 'O' || ev.ch == '\r')) {
            return;
        }
    }
}

void console_error(const char *msg) {
    char lines[ERR_MAX_LINES][ERR_LINE_MAX];
    const char *title = " ERROR ";
    const char *btn = "  OK  ";
    int nlines;
    int inner;
    int box_w;
    int box_h;
    int bx;
    int by;
    int i;
    int maxw;
    int title_x;
    int btn_w;
    int btn_x;
    int btn_y;
    int saved_color;

    if (console_is_capturing()) {
        console_write_line(msg ? msg : "");
        return;
    }

    serial_putchar('\n');
    serial_putchar('!');
    serial_putchar(' ');
    if (msg) {
        const char *p = msg;
        while (*p) {
            serial_putchar(*p++);
        }
    }
    serial_putchar('\n');

    maxw = term_cols - 10;
    if (maxw > ERR_LINE_MAX - 1) {
        maxw = ERR_LINE_MAX - 1;
    }
    if (maxw < 16) {
        maxw = 16;
    }
    nlines = err_wrap(msg, lines, maxw);

    inner = (int)strlen(title) + 2; /* bomb + gap in the title bar */
    if ((int)strlen(btn) + 4 > inner) {
        inner = (int)strlen(btn) + 4;
    }
    for (i = 0; i < nlines; i++) {
        int n = (int)strlen(lines[i]);
        if (n > inner) {
            inner = n;
        }
    }
    if (inner > maxw) {
        inner = maxw;
    }

    box_w = inner + 4;
    box_h = 6 + nlines;
    if (box_w > term_cols - 2) {
        box_w = term_cols - 2;
    }
    if (box_h > term_rows - 1) {
        box_h = term_rows - 1;
    }
    bx = (term_cols - box_w) / 2;
    by = (term_rows - box_h) / 2;
    if (bx < 0) {
        bx = 0;
    }
    if (by < 0) {
        by = 0;
    }

    saved_color = color;
    console_begin_direct();

    err_fill(0, 0, term_cols - 1, term_rows - 1, ' ', ERR_SCR_FG, ERR_SCR_BG);

    /* Drop shadow */
    err_fill(bx + 1, by + 1, bx + box_w, by + box_h, 0xB0u, ERR_SHAD_FG, ERR_SHAD_BG);

    err_fill(bx, by, bx + box_w - 1, by + box_h - 1, ' ', ERR_BOX_FG, ERR_BOX_BG);
    err_cell(bx, by, ERR_CH_TL, ERR_EDGE_FG, ERR_BOX_BG);
    err_cell(bx + box_w - 1, by, ERR_CH_TR, ERR_EDGE_FG, ERR_BOX_BG);
    err_cell(bx, by + box_h - 1, ERR_CH_BL, ERR_EDGE_FG, ERR_BOX_BG);
    err_cell(bx + box_w - 1, by + box_h - 1, ERR_CH_BR, ERR_EDGE_FG, ERR_BOX_BG);
    for (i = 1; i < box_w - 1; i++) {
        err_cell(bx + i, by, ERR_CH_H, ERR_EDGE_FG, ERR_BOX_BG);
        err_cell(bx + i, by + box_h - 1, ERR_CH_H, ERR_EDGE_FG, ERR_BOX_BG);
    }
    for (i = 1; i < box_h - 1; i++) {
        err_cell(bx, by + i, ERR_CH_V, ERR_EDGE_FG, ERR_BOX_BG);
        err_cell(bx + box_w - 1, by + i, ERR_CH_V, ERR_EDGE_FG, ERR_BOX_BG);
    }

    title_x = bx + (box_w - ((int)strlen(title) + 1)) / 2;
    if (title_x < bx + 1) {
        title_x = bx + 1;
    }
    err_cell(title_x, by, ERR_CH_BOMB, ERR_BOMB_FG, ERR_BOX_BG);
    err_puts(title_x + 1, by, title, ERR_BOX_FG, ERR_BOX_BG);

    for (i = 0; i < nlines; i++) {
        int lx = bx + 2;
        err_puts(lx, by + 2 + i, lines[i], ERR_BOX_FG, ERR_BOX_BG);
    }

    btn_w = (int)strlen(btn);
    btn_x = bx + (box_w - (btn_w + 2)) / 2;
    btn_y = by + box_h - 3;
    err_cell(btn_x + 1, btn_y + 1, ' ', ERR_SHAD_FG, 0);
    for (i = 0; i < btn_w; i++) {
        err_cell(btn_x + 2 + i, btn_y + 1, ' ', ERR_SHAD_FG, 0);
    }
    err_cell(btn_x, btn_y, '[', ERR_EDGE_FG, ERR_BOX_BG);
    err_puts(btn_x + 1, btn_y, btn, ERR_BTN_FG, ERR_BTN_BG);
    err_cell(btn_x + 1 + btn_w, btn_y, ']', ERR_EDGE_FG, ERR_BOX_BG);

    console_hit_clear();
    console_hit_add(btn_x, btn_y, btn_w + 2, 1, HIT_ENTER);
    err_wait_ok();
    console_hit_clear();

    console_end_direct();
    color = (uint8_t)saved_color;
}

#define XFER_SCR_FG   0
#define XFER_SCR_BG   7  /* light grey desktop */
#define XFER_BOX_FG  15
#define XFER_BOX_BG   1  /* dark blue box */
#define XFER_EDGE_FG 11  /* cyan frame */
#define XFER_LAB_FG  11
#define XFER_NAME_FG 15
#define XFER_TTL_FG  14
#define XFER_BAR_FG  15
#define XFER_BAR_EMP  8
#define XFER_STAT_FG 14
#define XFER_SHAD_FG  8
#define XFER_SHAD_BG  7

#define XFER_CH_BAR  0xDBu
#define XFER_CH_EMP  0xB0u

static int xfer_on;
static int xfer_bx;
static int xfer_by;
static int xfer_bw;
static int xfer_inner;
static int xfer_bar_x;
static int xfer_bar_y;
static int xfer_bar_w;
static int xfer_pct_x;
static int xfer_stat_y;
static uint8_t xfer_saved_color;

static void xfer_clip(char *out, int width, const char *s) {
    int n;
    int i;

    if (!s) {
        s = "";
    }
    n = (int)strlen(s);
    if (width < 1) {
        out[0] = 0;
        return;
    }
    if (n <= width) {
        for (i = 0; i < n; i++) {
            out[i] = s[i];
        }
        out[n] = 0;
        return;
    }
    if (width <= 3) {
        for (i = 0; i < width; i++) {
            out[i] = '.';
        }
        out[width] = 0;
        return;
    }
    out[0] = '.';
    out[1] = '.';
    out[2] = '.';
    memcpy(out + 3, s + n - (width - 3), (size_t)(width - 3));
    out[width] = 0;
}

static void xfer_row(int y, uint8_t fg, const char *label, const char *value) {
    char clip[80];
    int lab_n = (int)strlen(label);
    int name_w = xfer_inner - lab_n;
    int x = xfer_bx + 2;

    if (name_w < 4) {
        name_w = 4;
    }
    err_fill(x, y, xfer_bx + xfer_bw - 3, y, ' ', XFER_BOX_FG, XFER_BOX_BG);
    err_puts(x, y, label, XFER_LAB_FG, XFER_BOX_BG);
    xfer_clip(clip, name_w, value);
    err_puts(x + lab_n, y, clip, fg, XFER_BOX_BG);
}

void console_xfer_begin(const char *title, const char *src, const char *dst) {
    char head[24];
    int i;
    int box_h;
    int title_n;
    int title_x;
    int max_inner;

    if (xfer_on) {
        console_xfer_end();
    }
    if (console_is_capturing()) {
        return;
    }
    if (!title || !title[0]) {
        title = "COPY";
    }
    if (!src) {
        src = "";
    }
    if (!dst) {
        dst = "";
    }

    xfer_saved_color = color;
    console_begin_direct();
    console_set_cursor_visible(0);

    max_inner = term_cols - 10;
    if (max_inner > 56) {
        max_inner = 56;
    }
    if (max_inner < 24) {
        max_inner = term_cols > 8 ? term_cols - 8 : term_cols;
    }
    xfer_inner = max_inner;
    xfer_bw = xfer_inner + 4;
    box_h = 9;
    if (xfer_bw > term_cols - 2) {
        xfer_bw = term_cols - 2;
        xfer_inner = xfer_bw - 4;
    }
    if (box_h > term_rows - 1) {
        box_h = term_rows - 1;
    }
    xfer_bx = (term_cols - xfer_bw) / 2;
    xfer_by = (term_rows - box_h) / 2;
    if (xfer_bx < 0) {
        xfer_bx = 0;
    }
    if (xfer_by < 0) {
        xfer_by = 0;
    }

    xfer_bar_x = xfer_bx + 2;
    xfer_bar_y = xfer_by + 5;
    xfer_bar_w = xfer_inner - 6;
    if (xfer_bar_w < 8) {
        xfer_bar_w = 8;
    }
    if (xfer_bar_x + xfer_bar_w + 5 > xfer_bx + xfer_bw - 2) {
        xfer_bar_w = xfer_bx + xfer_bw - 2 - xfer_bar_x - 5;
        if (xfer_bar_w < 4) {
            xfer_bar_w = 4;
        }
    }
    xfer_pct_x = xfer_bar_x + xfer_bar_w + 1;
    xfer_stat_y = xfer_by + 6;

    err_fill(0, 0, term_cols - 1, term_rows - 1, ' ', XFER_SCR_FG, XFER_SCR_BG);
    err_fill(xfer_bx + 1, xfer_by + 1, xfer_bx + xfer_bw, xfer_by + box_h,
             0xB0u, XFER_SHAD_FG, XFER_SHAD_BG);
    err_fill(xfer_bx, xfer_by, xfer_bx + xfer_bw - 1, xfer_by + box_h - 1,
             ' ', XFER_BOX_FG, XFER_BOX_BG);

    err_cell(xfer_bx, xfer_by, ERR_CH_TL, XFER_EDGE_FG, XFER_BOX_BG);
    err_cell(xfer_bx + xfer_bw - 1, xfer_by, ERR_CH_TR, XFER_EDGE_FG, XFER_BOX_BG);
    err_cell(xfer_bx, xfer_by + box_h - 1, ERR_CH_BL, XFER_EDGE_FG, XFER_BOX_BG);
    err_cell(xfer_bx + xfer_bw - 1, xfer_by + box_h - 1, ERR_CH_BR,
             XFER_EDGE_FG, XFER_BOX_BG);
    for (i = 1; i < xfer_bw - 1; i++) {
        err_cell(xfer_bx + i, xfer_by, ERR_CH_H, XFER_EDGE_FG, XFER_BOX_BG);
        err_cell(xfer_bx + i, xfer_by + box_h - 1, ERR_CH_H, XFER_EDGE_FG, XFER_BOX_BG);
    }
    for (i = 1; i < box_h - 1; i++) {
        err_cell(xfer_bx, xfer_by + i, ERR_CH_V, XFER_EDGE_FG, XFER_BOX_BG);
        err_cell(xfer_bx + xfer_bw - 1, xfer_by + i, ERR_CH_V, XFER_EDGE_FG, XFER_BOX_BG);
    }

    head[0] = ' ';
    title_n = 1;
    while (*title && title_n + 2 < (int)sizeof(head)) {
        head[title_n++] = *title++;
    }
    head[title_n++] = ' ';
    head[title_n] = 0;
    title_x = xfer_bx + (xfer_bw - title_n) / 2;
    if (title_x < xfer_bx + 1) {
        title_x = xfer_bx + 1;
    }
    err_puts(title_x, xfer_by, head, XFER_TTL_FG, XFER_BOX_BG);

    xfer_row(xfer_by + 2, XFER_NAME_FG, "From  ", src);
    xfer_row(xfer_by + 3, XFER_NAME_FG, "To    ", dst);

    xfer_on = 1;
    console_xfer_progress(0, 1, "Starting...");
}

void console_xfer_progress(uint32_t done, uint32_t total, const char *status) {
    uint32_t filled;
    uint32_t pct;
    char pcts[8];
    int i;
    int n;

    if (!xfer_on) {
        return;
    }
    if (total == 0) {
        total = 1;
        done = 1;
    }
    if (done > total) {
        done = total;
    }
    filled = (uint32_t)(((uint64_t)xfer_bar_w * done) / total);
    pct = (uint32_t)(((uint64_t)done * 100ull) / total);
    if (pct > 100) {
        pct = 100;
    }

    for (i = 0; i < xfer_bar_w; i++) {
        if ((uint32_t)i < filled) {
            err_cell(xfer_bar_x + i, xfer_bar_y, XFER_CH_BAR, XFER_BAR_FG, XFER_BOX_BG);
        } else {
            err_cell(xfer_bar_x + i, xfer_bar_y, XFER_CH_EMP, XFER_BAR_EMP, XFER_BOX_BG);
        }
    }

    n = 0;
    pcts[n++] = (pct >= 100) ? '1' : ' ';
    pcts[n++] = (pct >= 10) ? (char)('0' + (pct / 10) % 10) : ' ';
    pcts[n++] = (char)('0' + (pct % 10));
    pcts[n++] = '%';
    pcts[n] = 0;
    err_puts(xfer_pct_x, xfer_bar_y, pcts, XFER_BAR_FG, XFER_BOX_BG);

    if (!status) {
        status = "";
    }
    err_fill(xfer_bx + 2, xfer_stat_y, xfer_bx + xfer_bw - 3, xfer_stat_y,
             ' ', XFER_BOX_FG, XFER_BOX_BG);
    {
        char clip[80];
        xfer_clip(clip, xfer_inner, status);
        err_puts(xfer_bx + 2, xfer_stat_y, clip, XFER_STAT_FG, XFER_BOX_BG);
    }
}

void console_xfer_end(void) {
    if (!xfer_on) {
        return;
    }
    xfer_on = 0;
    console_end_direct();
    color = xfer_saved_color;
}
