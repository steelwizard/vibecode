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

static void sync_hw_cursor(void) {
    uint16_t pos;
    if (fb_active) {
        return;
    }
    if (!direct_vga && !scroll_follow) {
        pos = (uint16_t)(term_cols * term_rows - 1);
    } else {
        pos = (uint16_t)(cursor_y * term_cols + cursor_x);
    }
    outb(VGA_CRTC_INDEX, 0x0F);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX, 0x0E);
    outb(VGA_CRTC_DATA, (uint8_t)((pos >> 8) & 0xFF));
}

static void scroll_drop_oldest(void) {
    if (scroll_lines <= 1) {
        return;
    }
    for (int i = 1; i < scroll_lines; i++) {
        for (int x = 0; x < term_cols; x++) {
            scrollback[i - 1][x] = scrollback[i][x];
        }
    }
    scroll_lines--;
    if (scroll_view_top > 0) {
        scroll_view_top--;
    }
    for (int x = 0; x < term_cols; x++) {
        scrollback[scroll_lines - 1][x] = blank_cell();
    }
}

static void scroll_ensure_line(int abs_line) {
    while (scroll_lines <= abs_line) {
        if (scroll_lines >= SCROLLBACK_MAX) {
            scroll_drop_oldest();
        }
        for (int x = 0; x < term_cols; x++) {
            scrollback[scroll_lines][x] = blank_cell();
        }
        scroll_lines++;
    }
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

static void console_redraw(void) {
    if (fb_active && framebuffer) {
        for (int y = 0; y < term_rows; y++) {
            int sl = scroll_view_top + y;
            for (int x = 0; x < term_cols; x++) {
                uint16_t cell = blank_cell();
                if (sl >= 0 && sl < scroll_lines) {
                    cell = scrollback[sl][x];
                }
                fb_draw_cell(x, y, cell);
            }
        }
        sync_hw_cursor();
        return;
    }

    for (int y = 0; y < term_rows; y++) {
        int sl = scroll_view_top + y;
        for (int x = 0; x < term_cols; x++) {
            if (sl >= 0 && sl < scroll_lines) {
                VGA_MEM[y * term_cols + x] = scrollback[sl][x];
            } else {
                VGA_MEM[y * term_cols + x] = blank_cell();
            }
        }
    }
    sync_hw_cursor();
}

static void scroll_write_cell(int abs_line, int x, uint16_t cell) {
    scroll_ensure_line(abs_line);
    if (x >= 0 && x < CONSOLE_MAX_COLS) {
        scrollback[abs_line][x] = cell;
    }
    if (!direct_vga) {
        int vy = abs_line - scroll_view_top;
        if (vy >= 0 && vy < term_rows && x >= 0 && x < term_cols) {
            if (fb_active) {
                fb_draw_cell(x, vy, cell);
            } else {
                VGA_MEM[vy * term_cols + x] = cell;
            }
        }
    }
}

static void scroll_newline(void) {
    cursor_x = 0;
    direct_wrap_pending = 0;
    int next_abs = scroll_view_top + cursor_y + 1;
    scroll_ensure_line(next_abs);

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
    if (cursor_y < term_rows) {
        return;
    }
    if (fb_active && framebuffer) {
        uint8_t *fb = (uint8_t *)(uintptr_t)framebuffer;
        uint32_t block = (uint32_t)(term_rows - 1) * 16U * fb_pitch;
        for (uint32_t i = 0; i < block; i++) {
            fb[i] = fb[i + 16U * fb_pitch];
        }
        for (int x = 0; x < term_cols; x++) {
            fb_draw_cell(x, term_rows - 1, blank_cell());
        }
    } else {
        for (int y = 1; y < term_rows; y++) {
            for (int x = 0; x < term_cols; x++) {
                VGA_MEM[(y - 1) * term_cols + x] = VGA_MEM[y * term_cols + x];
            }
        }
        for (int x = 0; x < term_cols; x++) {
            VGA_MEM[(term_rows - 1) * term_cols + x] = blank_cell();
        }
    }
    cursor_y = term_rows - 1;
    sync_hw_cursor();
}

static void putchar_at(char c, int x, int y) {
    uint16_t cell = (uint16_t)(c | ((uint16_t)color << 8));
    if (fb_active) {
        fb_draw_cell(x, y, cell);
        return;
    }
    VGA_MEM[y * term_cols + x] = cell;
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

    direct_wrap_pending = 0;

    if (direct_vga) {
        if (fb_active) {
            fb_clear_screen((color >> 4) & 0x0F);
        } else {
            for (i = 0; i < term_cols * term_rows; i++) {
                VGA_MEM[i] = blank_cell();
            }
        }
        cursor_x = 0;
        cursor_y = 0;
        sync_hw_cursor();
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

void console_begin_capture(char *buf, size_t cap) {
    capture_buf = buf;
    capture_cap = cap;
    capture_len = 0;
    if (cap > 0) {
        buf[0] = 0;
    }
}

size_t console_end_capture(void) {
    size_t n = capture_len;
    if (capture_buf && capture_cap > 0) {
        if (capture_len >= capture_cap) {
            capture_len = capture_cap - 1;
        }
        capture_buf[capture_len] = 0;
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

    cell = (uint16_t)(c | ((uint16_t)color << 8));

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

    cell = (uint16_t)(c | ((uint16_t)(((bg & 0x0F) << 4) | (fg & 0x0F)) << 8));

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
