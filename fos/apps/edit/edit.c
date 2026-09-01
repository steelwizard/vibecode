/*
 * edit.c — Nano-style text editor for FOS.
 *
 * Usage: edit [file]
 *   Ctrl+S  save          Ctrl+X  exit (prompts if unsaved and writable)
 *   Ctrl+C  copy          Ctrl+K  cut           Ctrl+V  paste
 *   Ctrl+A  select all    Shift+arrows/Home/End/PgUp/PgDn select
 *   Mouse: click to move, drag to select; right-click copies a selection
 *   or pastes if nothing is selected.
 */

#include "fos_api.h"

#define TEXT_MAX   32768
#define MAX_COLS   320
#define TAB_SPACES 4
#define FG_STATUS  0  /* black on teal */
#define BG_STATUS  3  /* VGA cyan/teal #00AAAA */
#define FG_TEXT    15
#define BG_TEXT    1  /* dark blue */
#define FG_GUTTER  9  /* bright blue */
#define FG_GUTTER_CUR 14
#define FG_TILDE   9
#define FG_SEL     0
#define BG_SEL     11 /* bright cyan — distinct from the teal bar */

static int cols = 80;
static int edit_rows = 23;
static int status_row = 23;

static char text[TEXT_MAX];
static int text_len;
static int cursor;
static int modified;
static char filename[256];
static int scroll_line;
static int scroll_col;
static int sel_mark = -1; /* other end of the selection; -1 = none */
static int mouse_left;
static int last_mx = -1;
static int last_my = -1;
static uint64_t last_edge_ms;

static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

static void my_strcpy(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = 0;
}

static void my_memmove(char *dst, const char *src, size_t n) {
    if (dst == src || n == 0) {
        return;
    }
    if (dst < src) {
        while (n--) {
            *dst++ = *src++;
        }
    } else {
        dst += n;
        src += n;
        while (n--) {
            *--dst = *--src;
        }
    }
}

static void append_str(char *dst, int *n, int cap, const char *s) {
    while (s && *s && *n < cap) {
        dst[(*n)++] = *s++;
    }
}

static void append_dec(char *dst, int *n, int cap, int v) {
    char tmp[12];
    int i = 0;

    if (v < 0) {
        if (*n < cap) {
            dst[(*n)++] = '-';
        }
        v = -v;
    }
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0 && *n < cap) {
        dst[(*n)++] = tmp[--i];
    }
}

static int line_index(int line) {
    int i = 0;
    int n = 0;
    while (i < text_len && n < line) {
        if (text[i++] == '\n') {
            n++;
        }
    }
    return i;
}

static int line_length(int row) {
    int start = line_index(row);
    int len = 0;
    while (start + len < text_len && text[start + len] != '\n') {
        len++;
    }
    return len;
}

static int count_lines(void) {
    int n = 1;
    for (int i = 0; i < text_len; i++) {
        if (text[i] == '\n') {
            n++;
        }
    }
    return n;
}

static int gutter_width(void) {
    int n = count_lines();
    int w = 1;

    if (n < 1) {
        n = 1;
    }
    while (n >= 10) {
        n /= 10;
        w++;
    }
    if (w < 4) {
        w = 4;
    }
    return w + 2;
}

static int text_cols(void) {
    int gw = gutter_width();
    int n = cols - gw;
    return n < 8 ? 8 : n;
}

static int has_sel(void) {
    return sel_mark >= 0 && sel_mark != cursor;
}

static int sel_lo(void) {
    return sel_mark < cursor ? sel_mark : cursor;
}

static int sel_hi(void) {
    return sel_mark < cursor ? cursor : sel_mark;
}

static void clear_sel(void) {
    sel_mark = -1;
}

static void cursor_to_row_col(int *row, int *col) {
    int r = 0;
    int c = 0;
    for (int i = 0; i < cursor && i < text_len; i++) {
        if (text[i] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
        }
    }
    *row = r;
    *col = c;
}

static void row_col_to_cursor(int row, int col) {
    int r = 0;
    int c = 0;
    int i = 0;
    while (i < text_len && r < row) {
        if (text[i++] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
        }
    }
    while (i < text_len && c < col && text[i] != '\n') {
        i++;
        c++;
    }
    cursor = i;
}

static void init_geometry(fos_api_t *api) {
    int c = 80;
    int r = 25;

    if (api->get_term_size) {
        api->get_term_size(&c, &r);
    }
    if (c < 20) {
        c = 20;
    }
    if (c > MAX_COLS) {
        c = MAX_COLS;
    }
    if (r < 4) {
        r = 4;
    }
    cols = c;
    status_row = r - 1;
    edit_rows = status_row;
}

static void paint_span(fos_api_t *api, int x, int y, int n, uint8_t fg, uint8_t bg, char ch) {
    char buf[MAX_COLS + 1];
    int i;

    if (n <= 0 || y < 0 || y >= status_row + 1) {
        return;
    }
    if (x < 0) {
        n += x;
        x = 0;
    }
    if (x + n > cols) {
        n = cols - x;
    }
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        buf[i] = ch;
    }
    buf[n] = 0;
    api->goto_xy(x, y);
    api->write_color(fg, bg, buf);
}

static void paint_run(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg,
                     const char *s, int n) {
    char buf[MAX_COLS + 1];
    int i;

    if (n <= 0) {
        return;
    }
    if (x + n > cols) {
        n = cols - x;
    }
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        char ch = s[i];
        buf[i] = (ch == '\t') ? ' ' : ch;
    }
    buf[n] = 0;
    api->goto_xy(x, y);
    api->write_color(fg, bg, buf);
}

/* Padded to the full width so the bar colour spans the whole row. */
static void status_bar(fos_api_t *api, const char *s) {
    char line[MAX_COLS + 1];
    int n = 0;

    while (s[n] && n < cols) {
        line[n] = s[n];
        n++;
    }
    while (n < cols) {
        line[n++] = ' ';
    }
    line[n] = 0;
    api->goto_xy(0, status_row);
    api->write_color(FG_STATUS, BG_STATUS, line);
}

static void ensure_visible(int row, int col) {
    int tc = text_cols();

    if (row < scroll_line) {
        scroll_line = row;
    }
    if (row >= scroll_line + edit_rows) {
        scroll_line = row - edit_rows + 1;
    }
    if (scroll_line < 0) {
        scroll_line = 0;
    }
    if (col < scroll_col) {
        scroll_col = col;
    }
    if (col >= scroll_col + tc) {
        scroll_col = col - tc + 1;
    }
    if (scroll_col < 0) {
        scroll_col = 0;
    }
}

static int in_sel(int pos) {
    return has_sel() && pos >= sel_lo() && pos < sel_hi();
}

static void fmt_gutter(char *out, int width, int num) {
    int dw = width - 2;
    int i;

    out[width] = 0;
    out[width - 2] = '|';
    out[width - 1] = ' ';
    for (i = dw - 1; i >= 0; i--) {
        out[i] = (char)('0' + (num % 10));
        num /= 10;
        if (num == 0) {
            while (i > 0) {
                out[--i] = ' ';
            }
            break;
        }
    }
}

static void draw(fos_api_t *api) {
    char status[MAX_COLS + 1];
    char gbuf[16];
    int row;
    int col;
    int total;
    int gw;
    int tc;
    int n;

    cursor_to_row_col(&row, &col);
    ensure_visible(row, col);
    total = count_lines();
    gw = gutter_width();
    tc = text_cols();

    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }

    for (int vr = 0; vr < edit_rows; vr++) {
        int sr = scroll_line + vr;
        int x = 0;

        if (sr >= total) {
            paint_span(api, 0, vr, gw, FG_TILDE, BG_TEXT, ' ');
            api->goto_xy(0, vr);
            api->write_color(FG_TILDE, BG_TEXT, "~");
            paint_span(api, 1, vr, cols - 1, FG_TEXT, BG_TEXT, ' ');
            continue;
        }

        fmt_gutter(gbuf, gw, sr + 1);
        api->goto_xy(0, vr);
        api->write_color(sr == row ? FG_GUTTER_CUR : FG_GUTTER, BG_TEXT, gbuf);
        x = gw;

        {
            int start = line_index(sr);
            int linelen = line_length(sr);
            int vis = linelen - scroll_col;
            int pos0 = start + scroll_col;
            int i = 0;
            int nl_pos = start + linelen;

            if (vis < 0) {
                vis = 0;
            }
            if (vis > tc) {
                vis = tc;
            }

            while (i < vis) {
                int sel = in_sel(pos0 + i);
                int run = 1;
                while (i + run < vis && in_sel(pos0 + i + run) == sel) {
                    run++;
                }
                if (sel) {
                    paint_run(api, x, vr, FG_SEL, BG_SEL, text + pos0 + i, run);
                } else {
                    paint_run(api, x, vr, FG_TEXT, BG_TEXT, text + pos0 + i, run);
                }
                x += run;
                i += run;
            }

            /* A selected newline shows as one highlighted cell past the text. */
            if (vis < tc && in_sel(nl_pos) && text[nl_pos] == '\n') {
                paint_span(api, x, vr, 1, FG_SEL, BG_SEL, ' ');
                x++;
            }
            if (x < cols) {
                paint_span(api, x, vr, cols - x, FG_TEXT, BG_TEXT, ' ');
            }
        }
    }

    n = 0;
    append_str(status, &n, cols, "^X quit  ^S save  ^C copy  ^K cut  ^V paste  ^A all  ");
    append_dec(status, &n, cols, row + 1);
    append_str(status, &n, cols, ":");
    append_dec(status, &n, cols, col + 1);
    if (modified) {
        append_str(status, &n, cols, "  * ");
    } else {
        append_str(status, &n, cols, "  ");
    }
    append_str(status, &n, cols, filename[0] ? filename : "[New file]");
    status[n] = 0;
    status_bar(api, status);

    if (api->set_cursor_visible) {
        api->set_cursor_visible(1);
    }
    {
        int cy = row - scroll_line;
        int cx = gw + col - scroll_col;
        if (cx < gw) {
            cx = gw;
        }
        if (cx > cols - 1) {
            cx = cols - 1;
        }
        if (cy >= 0 && cy < edit_rows) {
            api->goto_xy(cx, cy);
        } else {
            api->goto_xy(0, status_row);
        }
    }
}

static void delete_range(int a, int b) {
    if (a < 0) {
        a = 0;
    }
    if (b > text_len) {
        b = text_len;
    }
    if (b <= a) {
        clear_sel();
        return;
    }
    my_memmove(text + a, text + b, (size_t)(text_len - b));
    text_len -= (b - a);
    text[text_len] = 0;
    cursor = a;
    modified = 1;
    clear_sel();
}

static void insert_text(const char *s, int n) {
    int i;
    int w = 0;
    int o;

    if (has_sel()) {
        delete_range(sel_lo(), sel_hi());
    }
    if (!s || n <= 0) {
        return;
    }

    for (i = 0; i < n; i++) {
        if (s[i] == '\0') {
            continue;
        }
        if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') {
            continue;
        }
        w++;
    }
    if (text_len + w >= TEXT_MAX) {
        w = TEXT_MAX - 1 - text_len;
    }
    if (w <= 0) {
        return;
    }

    my_memmove(text + cursor + w, text + cursor, (size_t)(text_len - cursor));
    o = 0;
    for (i = 0; i < n && o < w; i++) {
        char c = s[i];
        if (c == '\0') {
            continue;
        }
        if (c == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') {
                continue;
            }
            c = '\n';
        }
        text[cursor + o] = c;
        o++;
    }
    if (o < w) {
        my_memmove(text + cursor + o, text + cursor + w, (size_t)(text_len - cursor));
    }
    cursor += o;
    text_len += o;
    text[text_len] = 0;
    modified = 1;
    clear_sel();
}

static void insert_char(fos_api_t *api, char c) {
    insert_text(&c, 1);
    draw(api);
}

static void backspace(fos_api_t *api) {
    if (has_sel()) {
        delete_range(sel_lo(), sel_hi());
        draw(api);
        return;
    }
    if (cursor <= 0) {
        return;
    }
    my_memmove(text + cursor - 1, text + cursor, (size_t)(text_len - cursor));
    cursor--;
    text_len--;
    text[text_len] = 0;
    modified = 1;
    draw(api);
}

static void delete_char(fos_api_t *api) {
    if (has_sel()) {
        delete_range(sel_lo(), sel_hi());
        draw(api);
        return;
    }
    if (cursor >= text_len) {
        return;
    }
    my_memmove(text + cursor, text + cursor + 1, (size_t)(text_len - cursor - 1));
    text_len--;
    text[text_len] = 0;
    modified = 1;
    draw(api);
}

static void begin_nav_sel(int shift) {
    if (shift) {
        if (sel_mark < 0) {
            sel_mark = cursor;
        }
    } else {
        clear_sel();
    }
}

static void move_left(fos_api_t *api, int shift) {
    int row;
    int col;

    begin_nav_sel(shift);
    if (cursor <= 0) {
        draw(api);
        return;
    }
    cursor_to_row_col(&row, &col);
    if (col == 0 && row > 0) {
        cursor = line_index(row - 1) + line_length(row - 1);
    } else {
        cursor--;
    }
    draw(api);
}

static void move_right(fos_api_t *api, int shift) {
    begin_nav_sel(shift);
    if (cursor < text_len) {
        cursor++;
    }
    draw(api);
}

static void move_up(fos_api_t *api, int shift) {
    int row;
    int col;

    begin_nav_sel(shift);
    cursor_to_row_col(&row, &col);
    if (row > 0) {
        row_col_to_cursor(row - 1, col);
    }
    draw(api);
}

static void move_down(fos_api_t *api, int shift) {
    int row;
    int col;
    int total = count_lines();

    begin_nav_sel(shift);
    cursor_to_row_col(&row, &col);
    if (row + 1 < total) {
        row_col_to_cursor(row + 1, col);
    }
    draw(api);
}

static void move_home(fos_api_t *api, int shift) {
    int row;
    int col;

    begin_nav_sel(shift);
    cursor_to_row_col(&row, &col);
    cursor = line_index(row);
    draw(api);
}

static void move_end(fos_api_t *api, int shift) {
    int row;
    int col;

    begin_nav_sel(shift);
    cursor_to_row_col(&row, &col);
    cursor = line_index(row) + line_length(row);
    draw(api);
}

static void move_page(fos_api_t *api, int dir, int shift) {
    int row;
    int col;
    int total = count_lines();
    int step = edit_rows > 1 ? edit_rows - 1 : 1;

    begin_nav_sel(shift);
    cursor_to_row_col(&row, &col);
    row += dir * step;
    if (row < 0) {
        row = 0;
    }
    if (row >= total) {
        row = total - 1;
    }
    row_col_to_cursor(row, col);
    draw(api);
}

static void select_all(fos_api_t *api) {
    sel_mark = 0;
    cursor = text_len;
    draw(api);
}

static void sel_or_line_range(int *a, int *b) {
    if (has_sel()) {
        *a = sel_lo();
        *b = sel_hi();
        return;
    }
    {
        int row;
        int col;
        cursor_to_row_col(&row, &col);
        *a = line_index(row);
        *b = *a + line_length(row);
        if (*b < text_len && text[*b] == '\n') {
            (*b)++;
        }
    }
}

static void copy_sel(fos_api_t *api) {
    int a;
    int b;

    sel_or_line_range(&a, &b);
    if (api->clip_set) {
        api->clip_set(text + a, (size_t)(b - a));
    }
}

static void cut_sel(fos_api_t *api) {
    int a;
    int b;

    sel_or_line_range(&a, &b);
    if (api->clip_set) {
        api->clip_set(text + a, (size_t)(b - a));
    }
    if (b > a) {
        delete_range(a, b);
    }
    draw(api);
}

static void paste_clip(fos_api_t *api) {
    static char buf[TEXT_MAX];
    size_t n;

    if (!api->clip_get) {
        return;
    }
    n = api->clip_get(buf, sizeof(buf));
    if (n == 0) {
        return;
    }
    insert_text(buf, (int)n);
    draw(api);
}

static void mouse_to_cursor(int mx, int my) {
    int gw = gutter_width();
    int total = count_lines();
    int row;
    int col;

    if (my < 0) {
        my = 0;
    }
    if (my >= edit_rows) {
        my = edit_rows - 1;
    }
    row = scroll_line + my;
    if (row >= total) {
        row = total - 1;
        if (row < 0) {
            row = 0;
        }
        col = line_length(row);
    } else {
        col = mx - gw + scroll_col;
        if (col < 0) {
            col = 0;
        }
    }
    row_col_to_cursor(row, col);
}

static void handle_mouse(fos_api_t *api) {
    fos_mouse_t m;
    int edge = 0;
    int changed = 0;
    uint64_t now = 0;

    if (!api->mouse_poll || !api->mouse_poll(&m)) {
        mouse_left = 0;
        return;
    }

    if (api->get_ticks_ms) {
        now = api->get_ticks_ms();
    }

    if (m.buttons & 1) {
        if (!mouse_left) {
            if (m.y >= 0 && m.y < edit_rows) {
                mouse_left = 1;
                mouse_to_cursor(m.x, m.y);
                sel_mark = cursor;
                last_mx = m.x;
                last_my = m.y;
                last_edge_ms = now;
                changed = 1;
            }
        } else {
            if (m.y <= 0 && scroll_line > 0) {
                edge = -1;
            } else if (m.y >= edit_rows - 1) {
                int total = count_lines();
                if (scroll_line + edit_rows < total) {
                    edge = 1;
                }
            }
            if (edge && now != 0 && now - last_edge_ms >= 80) {
                scroll_line += edge;
                last_edge_ms = now;
                changed = 1;
            }
            if (m.x != last_mx || m.y != last_my || changed) {
                mouse_to_cursor(m.x, m.y);
                last_mx = m.x;
                last_my = m.y;
                changed = 1;
            }
        }
    } else if (mouse_left) {
        mouse_left = 0;
        if (!has_sel()) {
            clear_sel();
        }
        last_mx = -1;
        last_my = -1;
        changed = 1;
    }

    if (m.pending & 2) {
        if (has_sel()) {
            copy_sel(api);
        } else {
            paste_clip(api);
            return;
        }
    }

    if (changed) {
        draw(api);
    }
}

static int save_file(fos_api_t *api) {
    if (filename[0] == 0) {
        return -1;
    }
    {
        int fd = api->fopen(filename, FOS_O_WRITE | FOS_O_CREATE | FOS_O_TRUNC);
        uint32_t put = 0;
        if (fd < 0 ||
            (text_len > 0 && (api->fwrite(fd, text, (uint32_t)text_len, &put) != 0 ||
                              put != (uint32_t)text_len)) ||
            api->fclose(fd) != 0) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            if (api->show_error) {
                api->show_error("Could not save file");
            }
            draw(api);
            return -1;
        }
    }
    modified = 0;
    draw(api);
    return 0;
}

static int prompt_filename(fos_api_t *api, const char *title, char *out, size_t out_sz) {
    char buf[256];
    size_t len = 0;
    size_t cur = 0;

    buf[0] = 0;
    for (;;) {
        char row[MAX_COLS + 1];
        int n = 0;

        for (const char *p = title; *p && n < cols; p++) {
            row[n++] = *p;
        }
        if (n < cols) {
            row[n++] = ':';
        }
        if (n < cols) {
            row[n++] = ' ';
        }
        for (const char *p = buf; *p && n < cols; p++) {
            row[n++] = *p;
        }
        row[n] = 0;
        status_bar(api, row);
        if (api->set_cursor_visible) {
            api->set_cursor_visible(1);
        }
        {
            int cx = (int)(my_strlen(title) + 2 + cur);
            if (cx >= cols) {
                cx = cols - 1;
            }
            if (cx < 0) {
                cx = 0;
            }
            api->goto_xy(cx, status_row);
        }

        while (!api->has_key()) {
            __asm__ volatile("pause");
        }

        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }
        if (ev.type == FOS_KEY_CHAR && ev.ch == 27) {
            return -1;
        }
        if (ev.type == FOS_KEY_ENTER) {
            if (len == 0) {
                continue;
            }
            my_strcpy(out, buf);
            return 0;
        }
        if (ev.type == FOS_KEY_BACKSPACE && cur > 0) {
            my_memmove(buf + cur - 1, buf + cur, len - cur + 1);
            cur--;
            len--;
            continue;
        }
        if (ev.type == FOS_KEY_DELETE && cur < len) {
            my_memmove(buf + cur, buf + cur + 1, len - cur);
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
        if (ev.type == FOS_KEY_HOME) {
            cur = 0;
            continue;
        }
        if (ev.type == FOS_KEY_END) {
            cur = len;
            continue;
        }
        if (ev.type == FOS_KEY_CHAR && ev.ch >= 32 && ev.ch <= 126) {
            if (ev.ch == ' ') {
                continue;
            }
            if (len + 1 >= sizeof(buf) || len + 1 >= out_sz) {
                continue;
            }
            my_memmove(buf + cur + 1, buf + cur, len - cur + 1);
            buf[cur++] = ev.ch;
            len++;
        }
    }
}

static int prompt_save_as(fos_api_t *api) {
    char path[256];
    char base[256];

    if (prompt_filename(api, "Save as (Esc cancel)", base, sizeof(base)) != 0) {
        draw(api);
        return -1;
    }
    if (base[0] == '\\') {
        my_strcpy(path, base);
    } else {
        path[0] = '\\';
        path[1] = 0;
        size_t n = 1;
        for (const char *p = base; *p && n + 1 < sizeof(path); p++) {
            path[n++] = *p;
        }
        path[n] = 0;
    }
    my_strcpy(filename, path);
    return save_file(api);
}

static int file_drive(fos_api_t *api) {
    if (filename[0] >= '0' && filename[0] <= '9' && filename[1] == ':') {
        return filename[0] - '0';
    }
    if (api->get_drive) {
        return api->get_drive();
    }
    return 0;
}

/* FAT32 can be written; exFAT (drive 1: in the default image) cannot. */
static int theoretically_writable(fos_api_t *api) {
    if (api->drive_writable) {
        return api->drive_writable(file_drive(api));
    }
    return 1;
}

/* 1 = leave the editor, 0 = stay. */
static int confirm_quit(fos_api_t *api) {
    if (!modified || !theoretically_writable(api)) {
        return 1;
    }
    status_bar(api, "Save changes?  Y save  N discard  Esc cancel");
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
    if (api->hit_clear) {
        api->hit_clear();
    }
    if (api->hit_add) {
        api->hit_add(15, status_row, 6, 1, FOS_HIT_Y);
        api->hit_add(23, status_row, 9, 1, FOS_HIT_N);
        api->hit_add(34, status_row, 10, 1, FOS_HIT_ESC);
    }
    for (;;) {
        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y')) {
            if (api->hit_clear) {
                api->hit_clear();
            }
            if (filename[0] == 0) {
                return prompt_save_as(api) == 0;
            }
            return save_file(api) == 0;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N')) {
            if (api->hit_clear) {
                api->hit_clear();
            }
            return 1;
        }
        if (ev.type == FOS_KEY_CHAR &&
            (ev.ch == 27 || ev.ch == 3 || ev.ch == 24)) {
            if (api->hit_clear) {
                api->hit_clear();
            }
            draw(api);
            return 0;
        }
    }
}

/* Pass the name through. vfs_resolve already joins cwd and parses 1:\file.
 * Prepending cwd here turned drive-qualified names into \1:\file. */
static void normalize_path(char *path, const char *arg) {
    if (!arg) {
        path[0] = 0;
        return;
    }
    while (*arg == ' ' || *arg == '\t') {
        arg++;
    }
    my_strcpy(path, arg);
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    size_t loaded = 0;
    int opened = 0;

    init_geometry(api);
    normalize_path(filename, api->cmdline);

    text_len = 0;
    cursor = 0;
    modified = filename[0] == 0;
    scroll_line = 0;
    scroll_col = 0;
    sel_mark = -1;

    if (filename[0] != 0) {
        int fd = api->fopen(filename, FOS_O_READ);
        uint32_t n = 0;
        if (fd >= 0 && api->fread(fd, text, (uint32_t)sizeof(text) - 1, &n) == 0) {
            loaded = n;
            text_len = (int)loaded;
            modified = 0;
            opened = 1;
        }
        if (fd >= 0) {
            api->fclose(fd);
        }
        if (!opened) {
            /* Named file that does not exist yet — treat as new. */
            modified = 1;
        }
    }
    text[text_len] = 0;

    draw(api);

    for (;;) {
        handle_mouse(api);
        if (!api->has_key()) {
            __asm__ volatile("pause");
            continue;
        }

        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }

        if (ev.type == FOS_KEY_CHAR) {
            if (ev.ch == 24) { /* Ctrl+X */
                if (!confirm_quit(api)) {
                    continue;
                }
                break;
            }
            if (ev.ch == 19) { /* Ctrl+S */
                if (filename[0] == 0) {
                    prompt_save_as(api);
                } else {
                    save_file(api);
                }
                continue;
            }
            if (ev.ch == 3) { /* Ctrl+C */
                copy_sel(api);
                continue;
            }
            if (ev.ch == 11) { /* Ctrl+K */
                cut_sel(api);
                continue;
            }
            if (ev.ch == 22) { /* Ctrl+V */
                paste_clip(api);
                continue;
            }
            if (ev.ch == 1) { /* Ctrl+A */
                select_all(api);
                continue;
            }
            if (ev.ch >= 32 && ev.ch <= 126) {
                insert_char(api, ev.ch);
            }
            continue;
        }

        if (ev.type == FOS_KEY_TAB) {
            insert_text("    ", TAB_SPACES);
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_ENTER) {
            insert_char(api, '\n');
            continue;
        }

        if (ev.type == FOS_KEY_BACKSPACE) {
            backspace(api);
            continue;
        }

        if (ev.type == FOS_KEY_DELETE) {
            if (ev.mods & FOS_MOD_SHIFT) {
                cut_sel(api);
            } else {
                delete_char(api);
            }
            continue;
        }

        {
            int shift = (ev.mods & FOS_MOD_SHIFT) != 0;
            if (ev.type == FOS_KEY_UP) {
                move_up(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_DOWN) {
                move_down(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_LEFT) {
                move_left(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_RIGHT) {
                move_right(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_HOME) {
                move_home(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_END) {
                move_end(api, shift);
                continue;
            }
            if (ev.type == FOS_KEY_PAGEUP) {
                move_page(api, -1, shift);
                continue;
            }
            if (ev.type == FOS_KEY_PAGEDOWN) {
                move_page(api, 1, shift);
                continue;
            }
        }
    }

    api->clear_screen();
    api->putchar('\n');
}
