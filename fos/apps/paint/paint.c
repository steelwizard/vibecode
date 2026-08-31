/*
 * paint.c — Cell-grid paint. Left-drag draws, right-drag erases.
 *
 *   paint [file.pnt]
 *
 * S / Ctrl+S save, O open, C clear, B fill tool, Q / Ctrl+X quit.
 * Click a swatch on the bottom row to change colour (or 0-9 / a-e).
 *
 * .PNT is a tiny binary: "FOSP" + cols + rows + paper + cells.
 */

#include "fos_api.h"

#define MAX_COLS  320
#define MAX_ROWS  90
#define PAPER     15
#define HDR_SIZE  8
#define FILE_MAX  (HDR_SIZE + MAX_COLS * MAX_ROWS)

#define FG_UI     15
#define BG_TITLE   1
#define BG_BAR     8

static int cols;
static int rows;
static int can_w;
static int can_h;
static uint8_t canvas[MAX_ROWS][MAX_COLS];
static uint8_t color = 0;
static int dirty;
static int drawing;
static int last_x;
static int last_y;
static char filename[256];
static uint8_t filebuf[FILE_MAX];
static int btn_c;
static int btn_s;
static int btn_o;
static int btn_q;
static int btn_f;
static int request_quit;
static int tool; /* 0 = draw, 1 = fill */
static int16_t fill_qx[MAX_COLS * MAX_ROWS];
static int16_t fill_qy[MAX_COLS * MAX_ROWS];

static size_t slen(const char *s) {
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        n++;
    }
    return n;
}

static void scpy(char *dst, const char *src, size_t cap) {
    size_t n = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

static int iabs(int v) {
    return v < 0 ? -v : v;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static int has_dot(const char *s) {
    while (*s) {
        if (*s == '.') {
            return 1;
        }
        s++;
    }
    return 0;
}

static void ensure_pnt(char *path, size_t cap) {
    size_t n;
    if (!path[0] || has_dot(path)) {
        return;
    }
    n = slen(path);
    if (n + 4 >= cap) {
        return;
    }
    path[n++] = '.';
    path[n++] = 'P';
    path[n++] = 'N';
    path[n++] = 'T';
    path[n] = 0;
}

static void normalize_path(fos_api_t *api, char *out, size_t cap, const char *arg) {
    char cwd[256];
    size_t n = 0;
    const char *p;

    arg = skip_ws(arg ? arg : "");
    if (!arg[0]) {
        out[0] = 0;
        return;
    }
    if (arg[0] == '\\') {
        scpy(out, arg, cap);
        ensure_pnt(out, cap);
        return;
    }
    cwd[0] = 0;
    if (api->get_cwd) {
        api->get_cwd(cwd, sizeof(cwd));
    }
    p = cwd[0] ? cwd : "\\";
    while (*p && n + 1 < cap) {
        out[n++] = *p++;
    }
    if (n == 0 || out[n - 1] != '\\') {
        if (n + 1 < cap) {
            out[n++] = '\\';
        }
    }
    while (*arg && n + 1 < cap) {
        out[n++] = *arg++;
    }
    out[n] = 0;
    ensure_pnt(out, cap);
}

static const char *short_name(const char *path) {
    const char *s = path;
    const char *last = path;
    if (!path || !path[0]) {
        return "[new]";
    }
    while (*s) {
        if (*s == '\\') {
            last = s + 1;
        }
        s++;
    }
    return last[0] ? last : path;
}

static void cell(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg, char ch) {
    if (x < 0 || y < 0 || x >= cols || y >= rows) {
        return;
    }
    api->goto_xy(x, y);
    api->set_color(fg, bg);
    api->putchar(ch);
}

static void hfill(fos_api_t *api, int y, uint8_t fg, uint8_t bg, char ch) {
    int x;
    for (x = 0; x < cols; x++) {
        cell(api, x, y, fg, bg, ch);
    }
}

static void put_str(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg, const char *s) {
    while (*s && x < cols) {
        cell(api, x++, y, fg, bg, *s++);
    }
}

static void plot(fos_api_t *api, int x, int y, uint8_t c) {
    if (x < 0 || y < 1 || x >= can_w || y > can_h) {
        return;
    }
    if (canvas[y][x] != c) {
        dirty = 1;
    }
    canvas[y][x] = c;
    cell(api, x, y, c == 0 ? 15 : 0, c, ' ');
}

static void line_to(fos_api_t *api, int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = iabs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        plot(api, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void clear_canvas(fos_api_t *api) {
    int y;
    int x;
    for (y = 1; y <= can_h; y++) {
        for (x = 0; x < can_w; x++) {
            canvas[y][x] = PAPER;
        }
    }
    dirty = filename[0] != 0;
    (void)api;
}

static void paint_canvas(fos_api_t *api) {
    int y;
    int x;
    for (y = 1; y <= can_h; y++) {
        for (x = 0; x < can_w; x++) {
            uint8_t c = canvas[y][x];
            cell(api, x, y, c == 0 ? 15 : 0, c, ' ');
        }
    }
}

static void paint_chrome(fos_api_t *api) {
    int x;
    int i;
    const char *name = short_name(filename);

    hfill(api, 0, FG_UI, BG_TITLE, ' ');
    put_str(api, 1, 0, 14, BG_TITLE, "PAINT");
    put_str(api, 8, 0, FG_UI, BG_TITLE, name);
    if (dirty) {
        put_str(api, 8 + (int)slen(name), 0, 12, BG_TITLE, " *");
    }
    put_str(api, cols > 40 ? cols - 22 : 20, 0, FG_UI, BG_TITLE, "ink ");
    cell(api, cols > 40 ? cols - 18 : 24, 0, color == 0 ? 15 : 0, color, ' ');
    cell(api, cols > 40 ? cols - 17 : 25, 0, color == 0 ? 15 : 0, color, ' ');
    put_str(api, cols > 40 ? cols - 15 : 28, 0, 11, BG_TITLE,
            tool ? "L fill  R erase" : "L draw  R erase");

    hfill(api, rows - 1, FG_UI, BG_BAR, ' ');
    for (i = 0; i < 16; i++) {
        uint8_t fg = (i == 0) ? 15 : 0;
        cell(api, i * 2, rows - 1, fg, (uint8_t)i, (color == i) ? '>' : ' ');
        cell(api, i * 2 + 1, rows - 1, fg, (uint8_t)i, ' ');
    }
    x = 33;
    btn_f = x;
    put_str(api, x, rows - 1, tool ? 0 : FG_UI, tool ? 14 : BG_BAR, "Fill ");
    x += 6;
    btn_c = x;
    put_str(api, x, rows - 1, 14, BG_BAR, "C");
    put_str(api, x + 1, rows - 1, FG_UI, BG_BAR, "lear ");
    x += 7;
    btn_s = x;
    put_str(api, x, rows - 1, 14, BG_BAR, "S");
    put_str(api, x + 1, rows - 1, FG_UI, BG_BAR, "ave ");
    x += 6;
    btn_o = x;
    put_str(api, x, rows - 1, 14, BG_BAR, "O");
    put_str(api, x + 1, rows - 1, FG_UI, BG_BAR, "pen ");
    x += 6;
    btn_q = x;
    put_str(api, x, rows - 1, 14, BG_BAR, "Q");
    put_str(api, x + 1, rows - 1, FG_UI, BG_BAR, "uit");
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

static void redraw(fos_api_t *api) {
    paint_canvas(api);
    paint_chrome(api);
}

static void status_msg(fos_api_t *api, const char *s) {
    hfill(api, rows - 1, FG_UI, BG_BAR, ' ');
    put_str(api, 1, rows - 1, 14, BG_BAR, s);
}

static int prompt_name(fos_api_t *api, const char *title, char *out, size_t cap) {
    char buf[256];
    size_t len = 0;
    size_t cur = 0;

    buf[0] = 0;
    if (filename[0]) {
        scpy(buf, short_name(filename), sizeof(buf));
        len = slen(buf);
        cur = len;
    }
    if (api->set_cursor_visible) {
        api->set_cursor_visible(1);
    }
    for (;;) {
        int n = 0;
        int x;
        hfill(api, rows - 1, FG_UI, BG_TITLE, ' ');
        put_str(api, 1, rows - 1, 14, BG_TITLE, title);
        n = 1 + (int)slen(title) + 1;
        put_str(api, n, rows - 1, FG_UI, BG_TITLE, buf);
        x = n + (int)cur;
        if (x >= cols) {
            x = cols - 1;
        }
        api->goto_xy(x, rows - 1);
        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        {
            fos_key_event_t ev = api->read_key();
            if (ev.type == FOS_KEY_CHAR && (ev.ch == 27 || ev.ch == 3)) {
                if (api->set_cursor_visible) {
                    api->set_cursor_visible(0);
                }
                return -1;
            }
            if (ev.type == FOS_KEY_ENTER) {
                if (len == 0) {
                    continue;
                }
                scpy(out, buf, cap);
                if (api->set_cursor_visible) {
                    api->set_cursor_visible(0);
                }
                return 0;
            }
            if (ev.type == FOS_KEY_BACKSPACE && cur > 0) {
                size_t i;
                for (i = cur - 1; i < len; i++) {
                    buf[i] = buf[i + 1];
                }
                cur--;
                len--;
                continue;
            }
            if (ev.type == FOS_KEY_DELETE && cur < len) {
                size_t i;
                for (i = cur; i < len; i++) {
                    buf[i] = buf[i + 1];
                }
                len--;
                continue;
            }
            if (ev.type == FOS_KEY_LEFT && cur > 0) {
                cur--;
                continue;
            }
            if (ev.type == FOS_KEY_RIGHT && cur < len) {
                cur++;
                continue;
            }
            if (ev.type == FOS_KEY_CHAR && ev.ch >= 32 && ev.ch <= 126) {
                size_t i;
                if (len + 1 >= sizeof(buf) || len + 1 >= cap) {
                    continue;
                }
                for (i = len + 1; i > cur; i--) {
                    buf[i] = buf[i - 1];
                }
                buf[cur++] = ev.ch;
                len++;
                buf[len] = 0;
            }
        }
    }
}

static int save_to(fos_api_t *api, const char *path) {
    size_t n = HDR_SIZE + (size_t)can_w * (size_t)can_h;
    int y;
    int x;
    uint8_t *p;

    if (n > FILE_MAX) {
        return -1;
    }
    filebuf[0] = 'F';
    filebuf[1] = 'O';
    filebuf[2] = 'S';
    filebuf[3] = 'P';
    filebuf[4] = (uint8_t)(can_w & 0xFF);
    filebuf[5] = (uint8_t)((can_w >> 8) & 0xFF);
    filebuf[6] = (uint8_t)(can_h & 0xFF);
    filebuf[7] = (uint8_t)((can_h >> 8) & 0xFF);
    p = filebuf + HDR_SIZE;
    for (y = 1; y <= can_h; y++) {
        for (x = 0; x < can_w; x++) {
            *p++ = canvas[y][x] & 0x0F;
        }
    }
    {
        int fd = api->fopen(path, FOS_O_WRITE | FOS_O_CREATE | FOS_O_TRUNC);
        uint32_t put = 0;
        if (fd < 0 || api->fwrite(fd, filebuf, (uint32_t)n, &put) != 0 || put != (uint32_t)n ||
            api->fclose(fd) != 0) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            return -1;
        }
    }
    dirty = 0;
    return 0;
}

static int load_from(fos_api_t *api, const char *path) {
    size_t got = 0;
    int fw;
    int fh;
    int y;
    int x;
    const uint8_t *p;

    {
        int fd = api->fopen(path, FOS_O_READ);
        uint32_t n = 0;
        if (fd < 0 || api->fread(fd, filebuf, (uint32_t)sizeof(filebuf), &n) != 0 ||
            n < HDR_SIZE) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            return -1;
        }
        api->fclose(fd);
        got = n;
    }
    if (filebuf[0] != 'F' || filebuf[1] != 'O' || filebuf[2] != 'S' || filebuf[3] != 'P') {
        return -1;
    }
    fw = filebuf[4] | (filebuf[5] << 8);
    fh = filebuf[6] | (filebuf[7] << 8);
    if (fw < 1 || fh < 1 || fw > MAX_COLS || fh > MAX_ROWS) {
        return -1;
    }
    if (got < HDR_SIZE + (size_t)fw * (size_t)fh) {
        return -1;
    }
    clear_canvas(api);
    p = filebuf + HDR_SIZE;
    for (y = 0; y < fh && y < can_h; y++) {
        for (x = 0; x < fw; x++) {
            uint8_t c = p[y * fw + x] & 0x0F;
            if (x < can_w) {
                canvas[y + 1][x] = c;
            }
        }
    }
    dirty = 0;
    return 0;
}

static int do_save(fos_api_t *api, int save_as) {
    char name[256];
    char path[256];

    if (!save_as && filename[0]) {
        if (save_to(api, filename) != 0) {
            if (api->show_error) {
                api->show_error("Could not save picture");
            }
            redraw(api);
            return -1;
        }
        paint_chrome(api);
        return 0;
    }
    if (prompt_name(api, "Save as:", name, sizeof(name)) != 0) {
        paint_chrome(api);
        return -1;
    }
    normalize_path(api, path, sizeof(path), name);
    scpy(filename, path, sizeof(filename));
    if (save_to(api, filename) != 0) {
        if (api->show_error) {
            api->show_error("Could not save picture");
        }
        filename[0] = 0;
        redraw(api);
        return -1;
    }
    paint_chrome(api);
    return 0;
}

static int do_open(fos_api_t *api) {
    char name[256];
    char path[256];

    if (prompt_name(api, "Open:", name, sizeof(name)) != 0) {
        paint_chrome(api);
        return -1;
    }
    normalize_path(api, path, sizeof(path), name);
    if (load_from(api, path) != 0) {
        if (api->show_error) {
            api->show_error("Not a FOS paint file (.PNT)");
        }
        redraw(api);
        return -1;
    }
    scpy(filename, path, sizeof(filename));
    redraw(api);
    return 0;
}

static int confirm_discard(fos_api_t *api) {
    fos_key_event_t ev;

    if (!dirty) {
        return 1;
    }
    status_msg(api, "Unsaved — Y save  N discard  Esc cancel");
    for (;;) {
        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y')) {
            return do_save(api, filename[0] == 0) == 0;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N')) {
            return 1;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 27 || ev.ch == 3)) {
            paint_chrome(api);
            return 0;
        }
    }
}

static int confirm_yes(fos_api_t *api, const char *msg) {
    fos_key_event_t ev;

    status_msg(api, msg);
    for (;;) {
        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y')) {
            return 1;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N' ||
                                        ev.ch == 27 || ev.ch == 3)) {
            paint_chrome(api);
            return 0;
        }
    }
}

static int on_canvas(int x, int y) {
    return x >= 0 && x < can_w && y >= 1 && y <= can_h;
}

static void flood_fill(fos_api_t *api, int x, int y, uint8_t nc) {
    uint8_t oc;
    int head = 0;
    int tail = 0;
    int maxq = can_w * can_h;

    if (!on_canvas(x, y)) {
        return;
    }
    oc = canvas[y][x];
    if (oc == nc) {
        return;
    }
    fill_qx[tail] = (int16_t)x;
    fill_qy[tail] = (int16_t)y;
    tail++;
    canvas[y][x] = nc;
    dirty = 1;
    while (head < tail) {
        int cx = fill_qx[head];
        int cy = fill_qy[head];
        int k;
        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        head++;
        for (k = 0; k < 4; k++) {
            int nx = cx + dx[k];
            int ny = cy + dy[k];
            if (!on_canvas(nx, ny) || canvas[ny][nx] != oc) {
                continue;
            }
            if (tail >= maxq) {
                break;
            }
            canvas[ny][nx] = nc;
            fill_qx[tail] = (int16_t)nx;
            fill_qy[tail] = (int16_t)ny;
            tail++;
        }
    }
    paint_canvas(api);
    paint_chrome(api);
}

static void handle_mouse(fos_api_t *api) {
    fos_mouse_t m;
    uint8_t ink;
    int was;

    if (!api->mouse_poll || !api->mouse_poll(&m)) {
        drawing = 0;
        return;
    }
    if (m.pending & 1) {
        if (m.y == rows - 1) {
            if (m.x < 32) {
                color = (uint8_t)(m.x / 2);
                paint_chrome(api);
            } else if (m.x >= btn_f && m.x < btn_f + 5) {
                tool = !tool;
                drawing = 0;
                paint_chrome(api);
            } else if (m.x >= btn_c && m.x < btn_c + 6) {
                if (confirm_yes(api, "Clear canvas? Y/N")) {
                    clear_canvas(api);
                    redraw(api);
                }
            } else if (m.x >= btn_s && m.x < btn_s + 5) {
                do_save(api, 0);
            } else if (m.x >= btn_o && m.x < btn_o + 5) {
                if (confirm_discard(api)) {
                    do_open(api);
                }
            } else if (m.x >= btn_q && m.x < btn_q + 4) {
                if (confirm_discard(api)) {
                    request_quit = 1;
                }
            }
        }
    }

    ink = (m.buttons & 2) ? PAPER : color;
    if (tool) {
        if ((m.buttons & 1) || (m.buttons & 2)) {
            if (on_canvas(m.x, m.y) && !drawing) {
                flood_fill(api, m.x, m.y, ink);
                drawing = 1;
            }
        } else {
            drawing = 0;
        }
        return;
    }
    if ((m.buttons & 1) || (m.buttons & 2)) {
        if (on_canvas(m.x, m.y)) {
            was = dirty;
            if (!drawing) {
                plot(api, m.x, m.y, ink);
                drawing = 1;
            } else {
                line_to(api, last_x, last_y, m.x, m.y, ink);
            }
            last_x = m.x;
            last_y = m.y;
            if (dirty && !was) {
                paint_chrome(api);
            }
        } else {
            drawing = 0;
        }
    } else {
        drawing = 0;
    }
}

static int hex_color(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    int quit = 0;

    if (api->begin_direct) {
        api->begin_direct();
    }
    cols = 80;
    rows = 25;
    if (api->get_term_size) {
        api->get_term_size(&cols, &rows);
    }
    if (cols < 40) {
        cols = 40;
    }
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows < 8) {
        rows = 8;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }
    can_w = cols;
    can_h = rows - 2;
    filename[0] = 0;
    color = 0;
    tool = 0;
    dirty = 0;
    drawing = 0;
    request_quit = 0;
    clear_canvas(api);

    normalize_path(api, filename, sizeof(filename), api->cmdline);
    if (filename[0]) {
        if (load_from(api, filename) != 0) {
            if (api->show_error) {
                api->show_error("Could not open picture — starting blank");
            }
            filename[0] = 0;
            clear_canvas(api);
        }
    }

    if (api->clear_screen) {
        api->clear_screen();
    }
    redraw(api);

    while (!quit) {
        while (!api->has_key() && !request_quit) {
            handle_mouse(api);
            __asm__ volatile("pause");
        }
        if (request_quit) {
            break;
        }
        {
            fos_key_event_t ev = api->read_key();
            int hc;
            if (ev.type == FOS_KEY_NONE) {
                continue;
            }
            if (ev.type != FOS_KEY_CHAR) {
                continue;
            }
            if (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 24 || ev.ch == 3) {
                quit = confirm_discard(api);
                continue;
            }
            if (ev.ch == 19 || ev.ch == 's') {
                do_save(api, 0);
                continue;
            }
            if (ev.ch == 'S') {
                do_save(api, 1);
                continue;
            }
            if (ev.ch == 'o' || ev.ch == 'O') {
                if (confirm_discard(api)) {
                    do_open(api);
                }
                continue;
            }
            if (ev.ch == 'b' || ev.ch == 'B') {
                tool = !tool;
                drawing = 0;
                paint_chrome(api);
                continue;
            }
            if (ev.ch == 'c' || ev.ch == 'C') {
                if (confirm_yes(api, "Clear canvas? Y/N")) {
                    clear_canvas(api);
                    dirty = filename[0] != 0;
                    redraw(api);
                }
                continue;
            }
            hc = hex_color(ev.ch);
            if (hc >= 0) {
                color = (uint8_t)hc;
                paint_chrome(api);
            }
        }
    }

    if (api->set_color) {
        api->set_color(15, 0);
    }
    if (api->set_cursor_visible) {
        api->set_cursor_visible(1);
    }
    if (api->end_direct) {
        api->end_direct();
    }
}
