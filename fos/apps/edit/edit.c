/*
 * edit.c — Simple nano-style text editor for FOS.
 *
 * Usage: edit <file>
 *   Ctrl+S  save
 *   Ctrl+X  exit
 *   Arrow keys / Home / End to move; Backspace/Delete edit at cursor.
 */

#include "fos_api.h"

#define TEXT_MAX   32768
#define MAX_COLS   320
#define FG_STATUS  15 /* white on blue */
#define BG_STATUS  1

/* Console geometry, filled from the kernel at startup. */
static int cols = 80;
static int edit_rows = 23;
static int status_row = 23;

static char text[TEXT_MAX];
static int text_len;
static int cursor;
static int modified;
static char filename[256];
static int scroll_line;

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

static void ensure_visible(int row) {
    if (row < scroll_line) {
        scroll_line = row;
    }
    if (row >= scroll_line + edit_rows) {
        scroll_line = row - edit_rows + 1;
    }
}

static void draw(fos_api_t *api) {
    char line[MAX_COLS + 1];
    char status[MAX_COLS + 1];
    int row;
    int col;

    cursor_to_row_col(&row, &col);
    ensure_visible(row);

    api->clear_screen();

    for (int vr = 0; vr < edit_rows; vr++) {
        int sr = scroll_line + vr;
        int start = line_index(sr);
        int i = 0;

        while (i < cols && start + i < text_len && text[start + i] != '\n') {
            line[i] = text[start + i];
            i++;
        }
        line[i] = 0;
        api->goto_xy(0, vr);
        api->write(line);
    }

    status[0] = 0;
    {
        const char *mod = modified ? " [Modified]" : "";
        const char *base = "^X Exit  ^S Save  Arrows/Home/End move";
        const char *shown = filename[0] ? filename : "[New file]";
        int n = 0;
        for (const char *p = base; *p && n < cols; p++) {
            status[n++] = *p;
        }
        for (const char *p = mod; *p && n < cols; p++) {
            status[n++] = *p;
        }
        if (n < cols) {
            status[n++] = ' ';
        }
        for (const char *p = shown; *p && n < cols; p++) {
            status[n++] = *p;
        }
        status[n] = 0;
    }
    status_bar(api, status);

    {
        int cy = row - scroll_line;
        int cx = col;
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

static void insert_char(fos_api_t *api, char c) {
    if (text_len + 1 >= TEXT_MAX) {
        return;
    }
    my_memmove(text + cursor + 1, text + cursor, (size_t)(text_len - cursor));
    text[cursor] = c;
    text_len++;
    cursor++;
    modified = 1;
    draw(api);
}

static void backspace(fos_api_t *api) {
    if (cursor <= 0) {
        return;
    }
    my_memmove(text + cursor - 1, text + cursor, (size_t)(text_len - cursor));
    cursor--;
    text_len--;
    modified = 1;
    draw(api);
}

static void delete_char(fos_api_t *api) {
    if (cursor >= text_len) {
        return;
    }
    my_memmove(text + cursor, text + cursor + 1, (size_t)(text_len - cursor - 1));
    text_len--;
    modified = 1;
    draw(api);
}

static void move_left(fos_api_t *api) {
    if (cursor <= 0) {
        return;
    }
    int row;
    int col;
    cursor_to_row_col(&row, &col);
    if (col == 0 && row > 0) {
        cursor = line_index(row - 1) + line_length(row - 1);
    } else {
        cursor--;
    }
    draw(api);
}

static void move_right(fos_api_t *api) {
    if (cursor >= text_len) {
        return;
    }
    cursor++;
    draw(api);
}

static void move_up(fos_api_t *api) {
    int row;
    int col;
    cursor_to_row_col(&row, &col);
    if (row > 0) {
        row_col_to_cursor(row - 1, col);
        ensure_visible(row - 1);
        draw(api);
    }
}

static void move_down(fos_api_t *api) {
    int row;
    int col;
    int total = count_lines();
    cursor_to_row_col(&row, &col);
    if (row + 1 < total) {
        row_col_to_cursor(row + 1, col);
        ensure_visible(row + 1);
        draw(api);
    }
}

static void move_home(fos_api_t *api) {
    int row;
    int col;
    cursor_to_row_col(&row, &col);
    cursor = line_index(row);
    draw(api);
}

static void move_end(fos_api_t *api) {
    int row;
    int col;
    cursor_to_row_col(&row, &col);
    cursor = line_index(row) + line_length(row);
    draw(api);
}

static int save_file(fos_api_t *api) {
    if (filename[0] == 0) {
        return -1;
    }
    if (api->write_file(filename, text, (size_t)text_len) != 0) {
        return -1;
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
        api->goto_xy((int)(my_strlen(title) + 2 + cur), status_row);

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
        if (ev.type == FOS_KEY_BACKSPACE) {
            if (cur > 0) {
                cur--;
                len--;
                buf[len] = 0;
            }
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

static void normalize_path(char *path, const char *arg) {
    if (!arg || !arg[0]) {
        path[0] = 0;
        return;
    }
    while (*arg == ' ' || *arg == '\t') {
        arg++;
    }
    if (arg[0] == '\\') {
        my_strcpy(path, arg);
        return;
    }
    path[0] = '\\';
    path[1] = 0;
    size_t n = my_strlen(path);
    for (const char *p = arg; *p && n + 1 < sizeof(filename); p++) {
        path[n++] = *p;
    }
    path[n] = 0;
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    size_t loaded = 0;

    init_geometry(api);
    normalize_path(filename, api->cmdline);

    text_len = 0;
    cursor = 0;
    modified = filename[0] == 0;
    scroll_line = 0;

    if (filename[0] != 0 &&
        api->read_file(filename, text, sizeof(text) - 1, &loaded) == 0) {
        text_len = (int)loaded;
        modified = 0;
    }
    text[text_len] = 0;

    draw(api);

    for (;;) {
        while (!api->has_key()) {
            __asm__ volatile("pause");
        }

        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }

        if (ev.type == FOS_KEY_CHAR) {
            if (ev.ch == 24) {
                break;
            }
            if (ev.ch == 19) {
                if (filename[0] == 0) {
                    prompt_save_as(api);
                } else {
                    save_file(api);
                }
                continue;
            }
            if (ev.ch >= 32 && ev.ch <= 126) {
                insert_char(api, ev.ch);
            }
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
            delete_char(api);
            continue;
        }

        if (ev.type == FOS_KEY_UP) {
            move_up(api);
            continue;
        }

        if (ev.type == FOS_KEY_DOWN) {
            move_down(api);
            continue;
        }

        if (ev.type == FOS_KEY_LEFT) {
            move_left(api);
            continue;
        }

        if (ev.type == FOS_KEY_RIGHT) {
            move_right(api);
            continue;
        }

        if (ev.type == FOS_KEY_HOME) {
            move_home(api);
            continue;
        }

        if (ev.type == FOS_KEY_END) {
            move_end(api);
            continue;
        }
    }

    api->clear_screen();
    api->putchar('\n');
}
