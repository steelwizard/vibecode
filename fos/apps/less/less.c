/*
 * less.c — Pager (less/more-style) for FOS with Markdown rendering.
 *
 *   less [file]          View a file (Markdown syntax is styled)
 *   type file | less     View piped text
 *
 * Keys: q quit, Space/PgDn/f page down, b/PgUp page up,
 *       Up/k line up, Down/j line down, g top, G bottom.
 */

#include "fos_api.h"

#define TEXT_MAX   65536
#define MAX_COLS   320

#define FG_NORMAL   7
#define FG_H1       15
#define FG_H2       11
#define FG_H3       10
#define FG_H4       3
#define FG_BOLD     15
#define FG_ITALIC   11
#define FG_CODE     10
#define FG_LINK     9
#define FG_QUOTE    8
#define FG_LIST     11
#define FG_DIM      8
#define FG_TABLE    7
#define FG_TABLEHDR 15
#define BG_NORMAL   0
#define BG_CODE     1

static char text[TEXT_MAX];
static int text_len;
static char title[256];
static int scroll_line;
static int total_lines;
static int md_mode;

/* Console geometry, filled from the kernel at startup. */
static int cols = 80;
static int page_rows = 23;
static int status_row = 23;

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
    page_rows = status_row;
}

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

static int my_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + ('a' - 'A'));
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + ('a' - 'A'));
        }
        if (ca != cb) {
            return (unsigned char)ca - (unsigned char)cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
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

static int line_start(int line) {
    int i = 0;
    int n = 0;
    while (i < text_len && n < line) {
        if (text[i++] == '\n') {
            n++;
        }
    }
    return i;
}

static int line_end_pos(int line) {
    int start = line_start(line);
    int i = start;
    while (i < text_len && text[i] != '\n') {
        i++;
    }
    return i;
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

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = my_strlen(s);
    size_t su = my_strlen(suffix);
    if (su > sl) {
        return 0;
    }
    return my_strcasecmp(s + sl - su, suffix) == 0;
}

static int detect_md(const char *path) {
    if (!path || !path[0]) {
        return 0; /* piped text: plain unless the caller named a .md file */
    }
    if (ends_with_ci(path, ".MD") || ends_with_ci(path, ".MARKDOWN")) {
        return 1;
    }
    if (ends_with_ci(path, "README.TXT")) {
        return 1;
    }
    return 0;
}

static int at_end(void) {
    return scroll_line + page_rows >= total_lines;
}

static void clamp_scroll(void) {
    int max_top = total_lines > page_rows ? total_lines - page_rows : 0;
    if (scroll_line < 0) {
        scroll_line = 0;
    }
    if (scroll_line > max_top) {
        scroll_line = max_top;
    }
}

static void pad_line(fos_api_t *api, int y, int x) {
    api->set_color(FG_NORMAL, BG_NORMAL);
    while (x < cols) {
        api->goto_xy(x, y);
        api->putchar(' ');
        x++;
    }
}

static void emit_char(fos_api_t *api, int y, int *x, uint8_t fg, uint8_t bg, char c) {
    if (*x >= cols) {
        return;
    }
    api->set_color(fg, bg);
    api->goto_xy(*x, y);
    api->putchar(c);
    (*x)++;
}

static void emit_str(fos_api_t *api, int y, int *x, uint8_t fg, uint8_t bg,
                     const char *s, int len) {
    for (int i = 0; i < len && s[i]; i++) {
        emit_char(api, y, x, fg, bg, s[i]);
    }
}

static int is_fence_line(const char *line, int len) {
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i + 2 >= len) {
        return 0;
    }
    return line[i] == '`' && line[i + 1] == '`' && line[i + 2] == '`';
}

static int in_codeblock(int line) {
    int in = 0;
    for (int l = 0; l < line; l++) {
        int start = line_start(l);
        int end = line_end_pos(l);
        if (is_fence_line(text + start, end - start)) {
            in = !in;
        }
    }
    return in;
}

static int is_hr(const char *line, int len) {
    int i = 0;
    char c = 0;
    int count = 0;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    while (i < len) {
        char ch = line[i];
        if (ch == ' ' || ch == '\t') {
            i++;
            continue;
        }
        if (ch != '-' && ch != '*' && ch != '_') {
            return 0;
        }
        if (c == 0) {
            c = ch;
        } else if (ch != c) {
            return 0;
        }
        count++;
        i++;
    }
    return count >= 3;
}

static int header_level(const char *line, int len, int *skip) {
    int i = 0;
    int spaces = 0;

    while (spaces < 3 && i < len && line[i] == ' ') {
        spaces++;
        i++;
    }
    if (i >= len || line[i] != '#') {
        return 0;
    }
    int level = 0;
    while (i < len && line[i] == '#') {
        level++;
        i++;
    }
    if (level == 0 || level > 6) {
        return 0;
    }
    if (i >= len || line[i] != ' ') {
        return 0;
    }
    while (i < len && line[i] == ' ') {
        i++;
    }
    *skip = i;
    return level;
}

static int is_table_sep(const char *line, int len) {
    int i = 0;
    int seen = 0;

    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    while (i < len) {
        char c = line[i];
        if (c == '|' || c == '-' || c == ':' || c == ' ') {
            if (c == '-') {
                seen = 1;
            }
            i++;
            continue;
        }
        return 0;
    }
    return seen;
}

static int is_table_row(const char *line, int len) {
    int i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    return i < len && line[i] == '|';
}

static void emit_inline(fos_api_t *api, int y, int *x, const char *line, int len,
                        uint8_t fg, uint8_t bg) {
    int i = 0;

    while (i < len && *x < cols) {
        char c = line[i];

        if (c == '`') {
            int j = i + 1;
            while (j < len && line[j] != '`') {
                j++;
            }
            if (j < len) {
                emit_char(api, y, x, FG_CODE, BG_NORMAL, ' ');
                for (int k = i + 1; k < j; k++) {
                    emit_char(api, y, x, FG_CODE, BG_NORMAL, line[k]);
                }
                emit_char(api, y, x, FG_CODE, BG_NORMAL, ' ');
                i = j + 1;
                continue;
            }
        }

        if (c == '*' && i + 1 < len && line[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < len && !(line[j] == '*' && line[j + 1] == '*')) {
                j++;
            }
            if (j + 1 < len) {
                for (int k = i + 2; k < j; k++) {
                    emit_char(api, y, x, FG_BOLD, bg, line[k]);
                }
                i = j + 2;
                continue;
            }
        }

        if (c == '_' && i + 1 < len && line[i + 1] == '_') {
            int j = i + 2;
            while (j + 1 < len && !(line[j] == '_' && line[j + 1] == '_')) {
                j++;
            }
            if (j + 1 < len) {
                for (int k = i + 2; k < j; k++) {
                    emit_char(api, y, x, FG_BOLD, bg, line[k]);
                }
                i = j + 2;
                continue;
            }
        }

        if (c == '*' || c == '_') {
            char marker = c;
            int j = i + 1;
            while (j < len && line[j] != marker) {
                j++;
            }
            if (j < len && j > i + 1) {
                for (int k = i + 1; k < j; k++) {
                    emit_char(api, y, x, FG_ITALIC, bg, line[k]);
                }
                i = j + 1;
                continue;
            }
        }

        if (c == '[') {
            int j = i + 1;
            while (j < len && line[j] != ']') {
                j++;
            }
            if (j < len && j + 1 < len && line[j + 1] == '(') {
                int k = j + 2;
                while (k < len && line[k] != ')') {
                    k++;
                }
                if (k < len) {
                    for (int t = i + 1; t < j; t++) {
                        emit_char(api, y, x, FG_LINK, bg, line[t]);
                    }
                    emit_char(api, y, x, FG_DIM, bg, ' ');
                    emit_char(api, y, x, FG_DIM, bg, '(');
                    for (int t = j + 2; t < k; t++) {
                        emit_char(api, y, x, FG_DIM, bg, line[t]);
                    }
                    emit_char(api, y, x, FG_DIM, bg, ')');
                    i = k + 1;
                    continue;
                }
            }
        }

        emit_char(api, y, x, fg, bg, c);
        i++;
    }
}

static void draw_md_line(fos_api_t *api, int y, int line_num) {
    int start = line_start(line_num);
    int end = line_end_pos(line_num);
    const char *line = text + start;
    int len = end - start;
    int x = 0;
    int skip = 0;
    int level;

    if (line_num >= total_lines) {
        pad_line(api, y, 0);
        return;
    }

    if (is_fence_line(line, len)) {
        pad_line(api, y, 0);
        return;
    }

    if (in_codeblock(line_num)) {
        emit_str(api, y, &x, FG_CODE, BG_CODE, line, len);
        pad_line(api, y, x);
        return;
    }

    if (is_hr(line, len)) {
        for (int i = 0; i < cols; i++) {
            emit_char(api, y, &x, FG_DIM, BG_NORMAL, '-');
        }
        return;
    }

    level = header_level(line, len, &skip);
    if (level > 0) {
        uint8_t hfg = FG_H4;
        if (level == 1) {
            hfg = FG_H1;
        } else if (level == 2) {
            hfg = FG_H2;
        } else if (level == 3) {
            hfg = FG_H3;
        }
        emit_inline(api, y, &x, line + skip, len - skip, hfg, BG_NORMAL);
        pad_line(api, y, x);
        return;
    }

    skip = 0;
    while (skip < len && (line[skip] == ' ' || line[skip] == '\t')) {
        skip++;
    }

    if (skip < len && line[skip] == '>') {
        skip++;
        while (skip < len && (line[skip] == ' ' || line[skip] == '\t')) {
            skip++;
        }
        emit_char(api, y, &x, FG_QUOTE, BG_NORMAL, '|');
        emit_char(api, y, &x, FG_QUOTE, BG_NORMAL, ' ');
        emit_inline(api, y, &x, line + skip, len - skip, FG_QUOTE, BG_NORMAL);
        pad_line(api, y, x);
        return;
    }

    if (skip < len && (line[skip] == '-' || line[skip] == '*' || line[skip] == '+')
        && skip + 1 < len && line[skip + 1] == ' ') {
        emit_char(api, y, &x, FG_LIST, BG_NORMAL, '*');
        emit_char(api, y, &x, FG_LIST, BG_NORMAL, ' ');
        emit_inline(api, y, &x, line + skip + 2, len - skip - 2, FG_NORMAL, BG_NORMAL);
        pad_line(api, y, x);
        return;
    }

    if (skip < len && line[skip] >= '0' && line[skip] <= '9') {
        int j = skip;
        while (j < len && line[j] >= '0' && line[j] <= '9') {
            j++;
        }
        if (j < len && line[j] == '.' && j + 1 < len && line[j + 1] == ' ') {
            for (int k = skip; k <= j; k++) {
                emit_char(api, y, &x, FG_LIST, BG_NORMAL, line[k]);
            }
            emit_char(api, y, &x, FG_LIST, BG_NORMAL, ' ');
            emit_inline(api, y, &x, line + j + 2, len - j - 2, FG_NORMAL, BG_NORMAL);
            pad_line(api, y, x);
            return;
        }
    }

    if (is_table_sep(line, len)) {
        for (int i = 0; i < len && x < cols; i++) {
            char c = line[i];
            uint8_t fg = (c == '-') ? FG_DIM : FG_TABLE;
            emit_char(api, y, &x, fg, BG_NORMAL, c);
        }
        pad_line(api, y, x);
        return;
    }

    if (is_table_row(line, len)) {
        uint8_t row_fg = FG_TABLE;
        int next = line_num + 1;
        if (next < total_lines) {
            int nstart = line_start(next);
            int nend = line_end_pos(next);
            if (is_table_sep(text + nstart, nend - nstart)) {
                row_fg = FG_TABLEHDR;
            }
        }
        emit_inline(api, y, &x, line, len, row_fg, BG_NORMAL);
        pad_line(api, y, x);
        return;
    }

    emit_inline(api, y, &x, line, len, FG_NORMAL, BG_NORMAL);
    pad_line(api, y, x);
}

static void draw_plain_line(fos_api_t *api, int y, int line_num) {
    char line[MAX_COLS + 1];
    int start = line_start(line_num);
    int i = 0;

    if (line_num >= total_lines) {
        line[0] = 0;
    } else {
        while (i < cols && start + i < text_len && text[start + i] != '\n') {
            line[i] = text[start + i];
            i++;
        }
        line[i] = 0;
    }
    api->goto_xy(0, y);
    api->set_color(FG_NORMAL, BG_NORMAL);
    api->write(line);
    pad_line(api, y, (int)my_strlen(line));
}

static void draw_status(fos_api_t *api, const char *hint) {
    char status[MAX_COLS + 1];
    int n = 0;

    for (const char *p = title; *p && n + 1 < cols; p++) {
        status[n++] = *p;
    }
    if (hint && hint[0]) {
        if (n + 1 < cols) {
            status[n++] = ' ';
        }
        for (const char *p = hint; *p && n + 1 < cols; p++) {
            status[n++] = *p;
        }
    }
    status[n] = 0;

    api->goto_xy(0, status_row);
    api->set_color(FG_NORMAL, BG_NORMAL);
    api->write(status);
    for (int i = n; i < cols; i++) {
        api->goto_xy(i, status_row);
        api->putchar(' ');
    }
}

static void draw(fos_api_t *api) {
    const char *hint = at_end() ? "(END)" : "";

    api->clear_screen();

    for (int vr = 0; vr < page_rows; vr++) {
        int sr = scroll_line + vr;
        if (md_mode) {
            draw_md_line(api, vr, sr);
        } else {
            draw_plain_line(api, vr, sr);
        }
    }

    draw_status(api, hint);
}

static void scroll_by(fos_api_t *api, int delta) {
    scroll_line += delta;
    clamp_scroll();
    draw(api);
}

static void scroll_page_down(fos_api_t *api) {
    if (at_end()) {
        draw(api);
        return;
    }
    scroll_by(api, page_rows);
}

static void scroll_page_up(fos_api_t *api) {
    scroll_by(api, -page_rows);
}

static void scroll_top(fos_api_t *api) {
    scroll_line = 0;
    draw(api);
}

static void scroll_bottom(fos_api_t *api) {
    scroll_line = total_lines > page_rows ? total_lines - page_rows : 0;
    draw(api);
}

static int load_content(fos_api_t *api) {
    char path[256];
    size_t loaded = 0;

    title[0] = 0;
    text_len = 0;
    scroll_line = 0;
    md_mode = 1;

    if (api->pipe_in_len > 0) {
        size_t n = api->pipe_in_len;
        if (n >= TEXT_MAX) {
            n = TEXT_MAX - 1;
        }
        for (size_t i = 0; i < n; i++) {
            text[i] = api->pipe_in[i];
        }
        text_len = (int)n;
        my_strcpy(title, "(pipe)");
        text[text_len] = 0;
        total_lines = count_lines();
        return 0;
    }

    normalize_path(path, api->cmdline);
    if (path[0] == 0) {
        return -1;
    }

    {
        int fd = api->fopen(path, FOS_O_READ);
        uint32_t n = 0;
        if (fd < 0 || api->fread(fd, text, (uint32_t)sizeof(text) - 1, &n) != 0) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            return -1;
        }
        api->fclose(fd);
        loaded = n;
    }
    text_len = (int)loaded;
    text[text_len] = 0;
    my_strcpy(title, path);
    md_mode = detect_md(path);
    total_lines = count_lines();
    return 0;
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;

    init_geometry(api);

    if (load_content(api) != 0) {
        if (api->show_error) {
            api->show_error("LESS: file required (or pipe input)");
        } else {
            api->write_line("LESS: file required (or pipe input)");
        }
        return;
    }

    draw(api);

    for (;;) {
        while (!api->has_key()) {
            fos_mouse_t m;
            if (api->mouse_poll && api->mouse_poll(&m) && (m.pending & 1)) {
                if (m.y < page_rows / 2) {
                    scroll_page_up(api);
                } else {
                    scroll_page_down(api);
                }
            }
            __asm__ volatile("pause");
        }

        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }

        if (ev.type == FOS_KEY_CHAR) {
            if (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27 || ev.ch == 3) {
                break;
            }
            if (ev.ch == ' ' || ev.ch == 'f' || ev.ch == 'F') {
                scroll_page_down(api);
                continue;
            }
            if (ev.ch == 'b' || ev.ch == 'B') {
                scroll_page_up(api);
                continue;
            }
            if (ev.ch == 'g') {
                scroll_top(api);
                continue;
            }
            if (ev.ch == 'G') {
                scroll_bottom(api);
                continue;
            }
            if (ev.ch == 'j') {
                scroll_by(api, 1);
                continue;
            }
            if (ev.ch == 'k') {
                scroll_by(api, -1);
                continue;
            }
            continue;
        }

        if (ev.type == FOS_KEY_ENTER) {
            scroll_by(api, 1);
            continue;
        }

        if (ev.type == FOS_KEY_DOWN) {
            scroll_by(api, 1);
            continue;
        }

        if (ev.type == FOS_KEY_UP) {
            scroll_by(api, -1);
            continue;
        }

        if (ev.type == FOS_KEY_PAGEDOWN) {
            scroll_page_down(api);
            continue;
        }

        if (ev.type == FOS_KEY_PAGEUP) {
            scroll_page_up(api);
            continue;
        }

        if (ev.type == FOS_KEY_HOME) {
            scroll_top(api);
            continue;
        }

        if (ev.type == FOS_KEY_END) {
            scroll_bottom(api);
            continue;
        }
    }

    api->clear_screen();
    api->putchar('\n');
}
