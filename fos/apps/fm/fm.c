/*
 * fm.c — Directory browser (ncurses-style TUI).
 *
 *   Up/Dn/PgUp/PgDn/Home/End  move selection
 *   Enter                     open directory, run a .COM / .BAT, or open
 *                             a .PNT in paint
 *   e                         edit file
 *   v                         view file
 *   n                         new empty file
 *   m / k                     mkdir
 *   c                         copy — browse to dest, s/Space drop
 *   r                         move / rename — same destination picker
 *   d                         delete
 *   0-3                       switch drive (same as 0: / 1: in the shell)
 *   Tab                       next drive
 *   q                         quit
 *   Esc                       cancel copy/move, or quit
 */

#include "fos_api.h"

#define MAX_COLS   320
#define ENTRY_MAX  128

#define C_BG       1  /* dark blue desktop */
#define C_FG      15
#define C_BR      11  /* cyan frames */
#define C_TITLE   14  /* yellow */
#define C_HELP    11
#define C_SEL_FG   0
#define C_SEL_BG  11  /* black on cyan highlight */
#define C_PARENT  10  /* green [..] */
#define C_FILE    13  /* magenta files */
#define C_DIR     15
#define C_SHADOW   0
#define C_ALERT_BG 4
#define C_ALERT_BR 14
#define C_ALERT_FG 15
#define C_OUT_BG   7  /* grey stdout screen */
#define C_OUT_FG   0
#define C_OUT_BR   1  /* blue border */
#define C_OUT_TTL  1
#define C_OUT_HINT 8
#define C_OUT_BTN_FG 15
#define C_OUT_BTN_BG 1

#define CH_TL  0xC9u
#define CH_TR  0xBBu
#define CH_BL  0xC8u
#define CH_BR  0xBCu
#define CH_H   0xCDu
#define CH_V   0xBAu

/* Console geometry, filled from the kernel at startup. */
static int cols = 80;
static int rows = 25;
static int list_rows = 20;
static int list_x = 2;
static int list_y = 6;
static int list_w = 76;

typedef struct {
    char name[64];
    int  is_dir;
    int  is_parent;
    uint32_t size;
} fm_entry_t;

static fm_entry_t entries[ENTRY_MAX];
static int entry_count;
static int selection;
static int list_scroll;
static char cwd[256];
static int pick_mode; /* 1 = copy, 2 = move */
static char pick_src[256];
static char pick_name[64];
static char pick_home[256];
static int pick_home_drive;

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

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static char up_ch(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 32);
    }
    return c;
}

static int my_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = up_ch(*a);
        char cb = up_ch(*b);
        if (ca != cb) {
            return (unsigned char)ca - (unsigned char)cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int is_ext_name(const char *name, const char *ext) {
    size_t n = my_strlen(name);
    size_t e = my_strlen(ext);

    if (n < e) {
        return 0;
    }
    return my_stricmp(name + n - e, ext) == 0;
}

static int is_runnable_name(const char *name) {
    return is_ext_name(name, ".COM") || is_ext_name(name, ".BAT");
}

static void join_path(char *out, size_t out_sz, const char *base, const char *name) {
    size_t n = 0;
    if (base[0] == '\\' && base[1] == 0) {
        out[n++] = '\\';
    } else {
        my_strcpy(out, base);
        n = my_strlen(out);
        if (n > 1 && out[n - 1] != '\\' && n + 1 < out_sz) {
            out[n++] = '\\';
        }
    }
    for (const char *p = name; *p && n + 1 < out_sz; p++) {
        out[n++] = *p;
    }
    out[n] = 0;
}

static void parent_path(const char *path, char *out, size_t out_sz) {
    (void)out_sz;
    my_strcpy(out, path);
    if (out[0] == '\\' && out[1] == 0) {
        return;
    }
    size_t n = my_strlen(out);
    while (n > 0 && out[n - 1] == '\\') {
        out[--n] = 0;
    }
    if (n == 0) {
        out[0] = '\\';
        out[1] = 0;
        return;
    }
    while (n > 1 && out[n - 1] != '\\') {
        n--;
    }
    if (n <= 1) {
        out[0] = '\\';
        out[1] = 0;
        return;
    }
    out[n] = 0;
}

static int cmp_entries(const fm_entry_t *a, const fm_entry_t *b) {
    if (a->is_parent != b->is_parent) {
        return a->is_parent ? -1 : 1;
    }
    if (a->is_dir != b->is_dir) {
        return b->is_dir - a->is_dir;
    }
    return my_strcmp(a->name, b->name);
}

static void sort_entries(void) {
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = i + 1; j < entry_count; j++) {
            if (cmp_entries(&entries[i], &entries[j]) > 0) {
                fm_entry_t tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static int reload_entries(fos_api_t *api) {
    fos_dir_t dir;
    char parent[256];

    entry_count = 0;
    api->get_cwd(cwd, sizeof(cwd));

    parent_path(cwd, parent, sizeof(parent));
    if (my_strcmp(cwd, parent) != 0) {
        my_strcpy(entries[0].name, "[..]");
        entries[0].is_dir = 1;
        entries[0].is_parent = 1;
        entries[0].size = 0;
        entry_count = 1;
    }

    if (api->read_dir(0, &dir) != 0) {
        return -1;
    }

    for (int i = 0; i < dir.count && entry_count < ENTRY_MAX; i++) {
        my_strcpy(entries[entry_count].name, dir.entries[i].name);
        entries[entry_count].is_dir = dir.entries[i].is_dir;
        entries[entry_count].is_parent = 0;
        entries[entry_count].size = dir.entries[i].size;
        entry_count++;
    }

    sort_entries();
    if (selection >= entry_count) {
        selection = entry_count > 0 ? entry_count - 1 : 0;
    }
    if (selection < 0) {
        selection = 0;
    }
    list_scroll = 0;
    return 0;
}

static void init_geometry(fos_api_t *api) {
    int c = 80;
    int r = 25;

    if (api->get_term_size) {
        api->get_term_size(&c, &r);
    }
    if (c < 40) {
        c = 40;
    }
    if (c > MAX_COLS) {
        c = MAX_COLS;
    }
    if (r < 12) {
        r = 12;
    }
    cols = c;
    rows = r;
    list_x = 2;
    list_y = 6;
    list_w = cols - 4;
    if (list_w < 8) {
        list_w = 8;
    }
    list_rows = rows - 8;
    if (list_rows < 1) {
        list_rows = 1;
    }
}

static void set_cursor(fos_api_t *api, int on) {
    if (api->set_cursor_visible) {
        api->set_cursor_visible(on);
    }
}

static void paint_ch(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg, unsigned char c) {
    char s[2];

    if (x < 0 || y < 0 || x >= cols || y >= rows) {
        return;
    }
    s[0] = (char)c;
    s[1] = 0;
    api->goto_xy(x, y);
    api->write_color(fg, bg, s);
}

static void paint_span(fos_api_t *api, int x, int y, int n, uint8_t fg, uint8_t bg, char ch) {
    char buf[MAX_COLS + 1];
    int i;

    if (n <= 0 || y < 0 || y >= rows) {
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

static void paint_text(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg,
                      const char *s, int maxn) {
    char buf[MAX_COLS + 1];
    int i = 0;

    if (!s || maxn <= 0) {
        return;
    }
    while (s[i] && i < maxn && i < MAX_COLS) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = 0;
    if (i > 0) {
        api->goto_xy(x, y);
        api->write_color(fg, bg, buf);
    }
}

static void draw_box(fos_api_t *api, int x, int y, int w, int h, uint8_t fg, uint8_t bg) {
    int i;

    if (w < 2 || h < 2) {
        return;
    }
    paint_ch(api, x, y, fg, bg, CH_TL);
    paint_span(api, x + 1, y, w - 2, fg, bg, (char)CH_H);
    paint_ch(api, x + w - 1, y, fg, bg, CH_TR);
    for (i = 1; i < h - 1; i++) {
        paint_ch(api, x, y + i, fg, bg, CH_V);
        paint_ch(api, x + w - 1, y + i, fg, bg, CH_V);
    }
    paint_ch(api, x, y + h - 1, fg, bg, CH_BL);
    paint_span(api, x + 1, y + h - 1, w - 2, fg, bg, (char)CH_H);
    paint_ch(api, x + w - 1, y + h - 1, fg, bg, CH_BR);
}

static void draw_box_fill(fos_api_t *api, int x, int y, int w, int h,
                         uint8_t fg, uint8_t bg) {
    int i;

    for (i = 0; i < h; i++) {
        paint_span(api, x, y + i, w, fg, bg, ' ');
    }
    draw_box(api, x, y, w, h, fg, bg);
}

static void box_title(fos_api_t *api, int x, int y, const char *title, uint8_t fg, uint8_t bg) {
    paint_text(api, x + 2, y, fg, bg, title, cols - x - 4);
}

static int fmt_u32(char *out, uint32_t v) {
    char tmp[11];
    int n = 0;
    int i;

    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = 0;
    return n;
}

static void ensure_visible(void) {
    if (selection < list_scroll) {
        list_scroll = selection;
    }
    if (selection >= list_scroll + list_rows) {
        list_scroll = selection - list_rows + 1;
    }
}

static void draw(fos_api_t *api) {
    const char *hint;
    const char *title;
    int i;

    ensure_visible();
    if (api->set_color) {
        api->set_color(C_FG, C_BG);
    }
    api->clear_screen();
    set_cursor(api, 0);

    draw_box(api, 0, 0, cols, rows, C_BR, C_BG);
    draw_box(api, 1, 1, cols - 2, 4, C_BR, C_BG);
    {
        char label[280];
        int d = api->get_drive ? api->get_drive() : 0;
        int n = 0;
        if (d < 0) {
            d = 0;
        }
        if (d > 9) {
            d = 9;
        }
        label[n++] = (char)('0' + d);
        label[n++] = ':';
        for (const char *p = cwd; *p && n + 1 < (int)sizeof(label); p++) {
            label[n++] = *p;
        }
        label[n] = 0;
        paint_text(api, 3, 2, C_TITLE, C_BG, "Path: ", 6);
        paint_text(api, 9, 2, C_TITLE, C_BG, label, cols - 12);
    }

    if (pick_mode) {
        hint = pick_mode == 2
                   ? "Move: Enter opens  s/Space drop  0-3 drive  Esc cancel"
                   : "Copy: Enter opens  s/Space drop  0-3 drive  Esc cancel";
        title = " Destination ";
    } else {
        hint = "Enter open/run  0-3 drive  c copy  r move  d del  v view  e edit  n new  m mkdir  q quit";
        title = " Browser ";
    }
    paint_text(api, 3, 3, C_HELP, C_BG, hint, cols - 6);

    draw_box(api, 1, 5, cols - 2, rows - 6, C_BR, C_BG);
    box_title(api, 1, 5, title, C_TITLE, C_BG);

    if (entry_count == 0) {
        paint_text(api, list_x, list_y, C_HELP, C_BG, "(nothing here)", list_w);
        return;
    }

    for (i = 0; i < list_rows; i++) {
        int idx = list_scroll + i;
        int y = list_y + i;
        uint8_t fg = C_DIR;
        uint8_t bg = C_BG;
        char sizebuf[16];
        int size_n;
        int name_w;
        const char *name;

        paint_span(api, list_x, y, list_w, C_FG, C_BG, ' ');
        if (idx >= entry_count) {
            continue;
        }

        if (idx == selection) {
            fg = C_SEL_FG;
            bg = C_SEL_BG;
            paint_span(api, list_x, y, list_w, fg, bg, ' ');
        } else if (entries[idx].is_parent) {
            fg = C_PARENT;
        } else if (!entries[idx].is_dir) {
            fg = C_FILE;
        }

        name = entries[idx].name;
        if (entries[idx].is_dir) {
            my_strcpy(sizebuf, "<DIR>");
            size_n = 5;
        } else {
            size_n = fmt_u32(sizebuf, entries[idx].size);
        }
        name_w = list_w - size_n - 1;
        if (name_w < 8) {
            name_w = list_w;
            size_n = 0;
        }
        paint_text(api, list_x, y, fg, bg, name, name_w);
        if (size_n > 0) {
            paint_text(api, list_x + list_w - size_n, y, fg, bg, sizebuf, size_n);
        }
    }
}

static void wait_key(fos_api_t *api) {
    while (!api->has_key()) {
        __asm__ volatile("pause");
    }
}

static void view_file(fos_api_t *api, const char *path, const char *title) {
    static char buf[8192];
    size_t len = 0;
    int scroll = 0;
    const int view_rows = list_rows > 1 ? list_rows : 1;

    {
        int fd = api->fopen(path, FOS_O_READ);
        uint32_t n = 0;
        if (fd < 0 || api->fread(fd, buf, (uint32_t)sizeof(buf) - 1, &n) != 0) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            return;
        }
        api->fclose(fd);
        len = n;
    }
    buf[len] = 0;

    for (;;) {
        int lines = 1;
        int shown = 0;
        int line_no = 0;
        size_t i = 0;

        for (size_t n = 0; n < len; n++) {
            if (buf[n] == '\n') {
                lines++;
            }
        }

        if (api->set_color) {
            api->set_color(C_FG, C_BG);
        }
        api->clear_screen();
        set_cursor(api, 0);
        draw_box(api, 0, 0, cols, rows, C_BR, C_BG);
        draw_box(api, 1, 1, cols - 2, rows - 2, C_BR, C_BG);
        box_title(api, 1, 1, " View ", C_TITLE, C_BG);
        paint_text(api, 3, 2, C_TITLE, C_BG, title, cols - 6);
        paint_text(api, 3, rows - 3, C_HELP, C_BG, "q close  Up/Dn/Pg page  Home/End", cols - 6);

        while (i < len && shown < view_rows) {
            char line[MAX_COLS + 1];
            int n = 0;

            if (line_no < scroll) {
                while (i < len && buf[i] != '\n') {
                    i++;
                }
                if (i < len) {
                    i++;
                }
                line_no++;
                continue;
            }
            while (i < len && buf[i] != '\n' && n < list_w) {
                char c = buf[i++];
                line[n++] = (c >= 32 && c <= 126) ? c : '.';
            }
            if (i < len && buf[i] == '\n') {
                i++;
            }
            line[n] = 0;
            paint_text(api, list_x, list_y + shown, C_FG, C_BG, line, list_w);
            shown++;
            line_no++;
        }

        wait_key(api);
        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27)) {
            return;
        }
        if (ev.type == FOS_KEY_HOME) {
            scroll = 0;
        } else if (ev.type == FOS_KEY_END) {
            scroll = lines - view_rows;
            if (scroll < 0) {
                scroll = 0;
            }
        } else if (ev.type == FOS_KEY_UP && scroll > 0) {
            scroll--;
        } else if (ev.type == FOS_KEY_DOWN && scroll + view_rows < lines) {
            scroll++;
        } else if (ev.type == FOS_KEY_PAGEUP) {
            scroll -= view_rows;
            if (scroll < 0) {
                scroll = 0;
            }
        } else if (ev.type == FOS_KEY_PAGEDOWN) {
            scroll += view_rows;
            if (scroll > lines - view_rows) {
                scroll = lines - view_rows;
            }
            if (scroll < 0) {
                scroll = 0;
            }
        }
    }
}

static int prompt_text(fos_api_t *api, const char *title, char *out, size_t out_sz,
                       int allow_slash, const char *seed) {
    char buf[256];
    size_t len = 0;
    size_t cur = 0;
    int dlg_w;
    int dlg_h = 9;
    int dlg_x;
    int dlg_y;
    int field_w;

    buf[0] = 0;
    if (seed) {
        while (seed[len] && len + 1 < sizeof(buf) && len + 1 < out_sz) {
            buf[len] = seed[len];
            len++;
        }
        buf[len] = 0;
        cur = len;
    }

    dlg_w = cols - 8;
    if (dlg_w > 64) {
        dlg_w = 64;
    }
    if (dlg_w < 28) {
        dlg_w = cols > 30 ? cols - 4 : cols;
    }
    dlg_x = (cols - dlg_w) / 2;
    dlg_y = (rows - dlg_h) / 2;
    if (dlg_x < 1) {
        dlg_x = 1;
    }
    if (dlg_y < 1) {
        dlg_y = 1;
    }
    field_w = dlg_w - 4;
    if (field_w < 8) {
        field_w = 8;
    }

    for (;;) {
        int cx;
        size_t disp0 = 0;

        draw(api);
        draw_box_fill(api, dlg_x + 2, dlg_y + 1, dlg_w, dlg_h, C_FG, C_SHADOW);
        draw_box_fill(api, dlg_x, dlg_y, dlg_w, dlg_h, C_FG, C_BG);
        draw_box(api, dlg_x, dlg_y, dlg_w, dlg_h, C_BR, C_BG);
        paint_text(api, dlg_x + 2, dlg_y + 1, C_TITLE, C_BG, title, dlg_w - 4);
        paint_text(api, dlg_x + 2, dlg_y + 2, C_HELP, C_BG, "Esc cancel  Enter confirm", dlg_w - 4);
        paint_span(api, dlg_x + 2, dlg_y + 4, field_w, C_TITLE, C_BG, ' ');

        if ((int)len > field_w) {
            disp0 = len - (size_t)field_w;
            if (cur < disp0) {
                disp0 = cur;
            }
        }
        paint_text(api, dlg_x + 2, dlg_y + 4, C_TITLE, C_BG, buf + disp0, field_w);

        cx = dlg_x + 2 + (int)(cur - disp0);
        if (cx < dlg_x + 2) {
            cx = dlg_x + 2;
        }
        if (cx > dlg_x + 2 + field_w - 1) {
            cx = dlg_x + 2 + field_w - 1;
        }
        set_cursor(api, 1);
        api->goto_xy(cx, dlg_y + 4);

        wait_key(api);
        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && ev.ch == 27) {
            set_cursor(api, 0);
            return -1;
        }
        if (ev.type == FOS_KEY_ENTER) {
            if (len == 0) {
                continue;
            }
            my_strcpy(out, buf);
            set_cursor(api, 0);
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
            if (!allow_slash && (ev.ch == '\\' || ev.ch == '/')) {
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

static int prompt_name(fos_api_t *api, const char *title, char *out, size_t out_sz) {
    return prompt_text(api, title, out, out_sz, 0, 0);
}

static int confirm_delete(fos_api_t *api, const char *name) {
    int dlg_w = cols - 8;
    int dlg_h = 7;
    int dlg_x;
    int dlg_y;

    if (dlg_w > 56) {
        dlg_w = 56;
    }
    if (dlg_w < 28) {
        dlg_w = cols > 32 ? cols - 4 : cols;
    }
    dlg_x = (cols - dlg_w) / 2;
    dlg_y = (rows - dlg_h) / 2;
    if (dlg_x < 1) {
        dlg_x = 1;
    }
    if (dlg_y < 1) {
        dlg_y = 1;
    }

    draw(api);
    set_cursor(api, 0);
    draw_box_fill(api, dlg_x + 2, dlg_y + 1, dlg_w, dlg_h, C_ALERT_FG, C_SHADOW);
    draw_box_fill(api, dlg_x, dlg_y, dlg_w, dlg_h, C_ALERT_FG, C_ALERT_BG);
    draw_box(api, dlg_x, dlg_y, dlg_w, dlg_h, C_ALERT_BR, C_ALERT_BG);
    paint_text(api, dlg_x + 2, dlg_y + 1, C_ALERT_BR, C_ALERT_BG, "Delete this file?", dlg_w - 4);
    paint_text(api, dlg_x + 2, dlg_y + 3, C_ALERT_FG, C_ALERT_BG, name, dlg_w - 4);
    paint_text(api, dlg_x + 2, dlg_y + 5, C_ALERT_FG, C_ALERT_BG, "y delete  n cancel", dlg_w - 4);
    if (api->hit_clear) {
        api->hit_clear();
    }
    if (api->hit_add) {
        api->hit_add(dlg_x + 2, dlg_y + 5, 8, 1, FOS_HIT_Y);
        api->hit_add(dlg_x + 12, dlg_y + 5, 8, 1, FOS_HIT_N);
    }

    for (;;) {
        wait_key(api);
        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'y' || ev.ch == 'Y')) {
            return 1;
        }
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'n' || ev.ch == 'N' || ev.ch == 27)) {
            return 0;
        }
    }
}

static void show_result(fos_api_t *api, const char *msg) {
    if (api->show_error) {
        api->show_error(msg);
        return;
    }
    draw(api);
    paint_text(api, list_x, rows - 3, C_TITLE, C_BG, msg, list_w);
    wait_key(api);
    (void)api->read_key();
}

static fm_entry_t *selected_entry(void) {
    if (entry_count == 0) {
        return 0;
    }
    if (entries[selection].is_parent) {
        return 0;
    }
    return &entries[selection];
}

static void selected_path(char *out, size_t out_sz) {
    join_path(out, out_sz, cwd, entries[selection].name);
}

static void qualify_path(fos_api_t *api, char *out, size_t out_sz, const char *path) {
    int d = api->get_drive ? api->get_drive() : 0;
    size_t n = 0;

    if (!path) {
        path = "\\";
    }
    if (path[0] >= '0' && path[0] <= '9' && path[1] == ':') {
        my_strcpy(out, path);
        return;
    }
    if (d < 0) {
        d = 0;
    }
    if (d > 9) {
        d = 9;
    }
    if (out_sz < 4) {
        out[0] = 0;
        return;
    }
    out[n++] = (char)('0' + d);
    out[n++] = ':';
    while (*path && n + 1 < out_sz) {
        out[n++] = *path++;
    }
    out[n] = 0;
}

static void switch_drive(fos_api_t *api, int d) {
    if (!api->set_drive) {
        show_result(api, "Need a newer kernel to change drives");
        draw(api);
        return;
    }
    if (api->get_drive && api->get_drive() == d) {
        return;
    }
    if (api->set_drive(d) != 0) {
        show_result(api, "No such drive");
        draw(api);
        return;
    }
    selection = 0;
    list_scroll = 0;
    if (reload_entries(api) != 0) {
        entry_count = 0;
    }
    draw(api);
}

static void start_pick(fos_api_t *api, int moving) {
    fm_entry_t *e = selected_entry();
    char raw[256];

    if (!e || e->is_dir) {
        show_result(api, moving ? "Move: pick a file" : "Copy: pick a file");
        draw(api);
        return;
    }
    if ((moving && !api->move_file) || (!moving && !api->copy_file)) {
        show_result(api, "Need a newer kernel for copy/move");
        draw(api);
        return;
    }

    selected_path(raw, sizeof(raw));
    qualify_path(api, pick_src, sizeof(pick_src), raw);
    my_strcpy(pick_name, e->name);
    my_strcpy(pick_home, cwd);
    pick_home_drive = api->get_drive ? api->get_drive() : 0;
    pick_mode = moving ? 2 : 1;
    draw(api);
}

static void cancel_pick(fos_api_t *api) {
    if (!pick_mode) {
        return;
    }
    pick_mode = 0;
    if (api->set_drive) {
        api->set_drive(pick_home_drive);
    }
    if (api->set_cwd(pick_home) == 0) {
        reload_entries(api);
    }
    draw(api);
}

static void drop_here(fos_api_t *api) {
    char name[64];
    char raw[256];
    char dst[256];
    int rc;
    int moving = pick_mode == 2;

    if (prompt_text(api, "Destination file name", name, sizeof(name), 0, pick_name) != 0) {
        draw(api);
        return;
    }

    join_path(raw, sizeof(raw), cwd, name);
    qualify_path(api, dst, sizeof(dst), raw);
    rc = moving ? api->move_file(pick_src, dst) : api->copy_file(pick_src, dst);
    pick_mode = 0;
    if (rc == -2) {
        show_result(api, "Out of memory");
    } else if (rc == -3) {
        show_result(api, "Copied, but could not remove the original");
        reload_entries(api);
    } else if (rc != 0) {
        show_result(api, moving ? "Move failed (FAT32 files only)" : "Copy failed (FAT32 files only)");
    } else {
        reload_entries(api);
    }
    draw(api);
}

static void delete_selected(fos_api_t *api) {
    fm_entry_t *e = selected_entry();
    char path[256];
    int rc;

    if (!e) {
        draw(api);
        return;
    }
    if (!api->delete_file) {
        show_result(api, "Need a newer kernel for delete");
        draw(api);
        return;
    }

    selected_path(path, sizeof(path));
    if (!confirm_delete(api, e->name)) {
        draw(api);
        return;
    }

    rc = api->delete_file(path);
    if (rc == -2) {
        show_result(api, "Directory not empty");
    } else if (rc != 0) {
        show_result(api, "Delete failed (FAT32 only)");
    } else {
        reload_entries(api);
    }
    draw(api);
}

static void drain_keys(fos_api_t *api) {
    int n = 0;
    if (!api->has_key || !api->read_key) {
        return;
    }
    while (n < 32 && api->has_key()) {
        (void)api->read_key();
        n++;
    }
}

/* After a nested .COM: drop leftover keys/audio, put VFS back. */
static void resume_after_com(fos_api_t *api, int saved_drive, const char *saved_cwd) {
    drain_keys(api);
    if (api->sound_stop) {
        api->sound_stop();
    }
    if (api->set_drive) {
        api->set_drive(saved_drive);
    }
    if (saved_cwd && saved_cwd[0] && api->set_cwd(saved_cwd) != 0) {
        /* Child may have removed this folder; stay where VFS landed. */
    }
    if (reload_entries(api) != 0) {
        entry_count = 0;
    }
}

static int output_has_body(const char *s) {
    while (s && *s) {
        char c = *s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return 1;
        }
    }
    return 0;
}

static int output_line_count(const char *s, int width) {
    int lines = 0;
    int col = 0;
    int any = 0;

    if (width < 1) {
        width = 1;
    }
    while (*s) {
        if (*s == '\r') {
            s++;
            continue;
        }
        any = 1;
        if (*s == '\n') {
            lines++;
            col = 0;
            s++;
            continue;
        }
        col++;
        s++;
        if (col >= width) {
            lines++;
            col = 0;
        }
    }
    if (col > 0 || !any) {
        lines++;
    }
    return lines;
}

static const char *output_skip_lines(const char *s, int skip, int width) {
    int line = 0;
    int col = 0;

    if (width < 1) {
        width = 1;
    }
    while (*s && line < skip) {
        if (*s == '\r') {
            s++;
            continue;
        }
        if (*s == '\n') {
            s++;
            line++;
            col = 0;
            continue;
        }
        col++;
        s++;
        if (col >= width) {
            line++;
            col = 0;
        }
    }
    return s;
}

static void show_com_output(fos_api_t *api, const char *title, const char *text) {
    int scroll = 0;
    int box_x = 2;
    int box_y = 1;
    int box_w = cols - 4;
    int box_h = rows - 2;
    int text_w;
    int text_h;
    int lines;
    const char *btn = "  OK  ";
    int btn_n = 6;

    if (box_w < 24) {
        box_w = cols > 4 ? cols - 2 : cols;
        box_x = (cols - box_w) / 2;
    }
    if (box_h < 8) {
        box_h = rows > 2 ? rows - 2 : rows;
        box_y = 0;
    }
    text_w = box_w - 4;
    text_h = box_h - 6;
    if (text_w < 8) {
        text_w = 8;
    }
    if (text_h < 1) {
        text_h = 1;
    }
    lines = output_line_count(text, text_w);

    set_cursor(api, 0);
    for (;;) {
        const char *p;
        int shown = 0;
        int btn_x;
        int btn_y;
        int max_scroll;

        max_scroll = lines - text_h;
        if (max_scroll < 0) {
            max_scroll = 0;
        }
        if (scroll > max_scroll) {
            scroll = max_scroll;
        }
        if (scroll < 0) {
            scroll = 0;
        }

        if (api->set_color) {
            api->set_color(C_OUT_FG, C_OUT_BG);
        }
        api->clear_screen();
        draw_box_fill(api, box_x + 1, box_y + 1, box_w, box_h, C_OUT_FG, C_SHADOW);
        draw_box_fill(api, box_x, box_y, box_w, box_h, C_OUT_FG, C_OUT_BG);
        draw_box(api, box_x, box_y, box_w, box_h, C_OUT_BR, C_OUT_BG);
        box_title(api, box_x, box_y, " Output ", C_OUT_TTL, C_OUT_BG);
        paint_text(api, box_x + 2, box_y + 1, C_OUT_TTL, C_OUT_BG, title, text_w);

        p = output_skip_lines(text, scroll, text_w);
        while (*p && shown < text_h) {
            char line[MAX_COLS + 1];
            int n = 0;

            while (*p == '\r') {
                p++;
            }
            while (*p && *p != '\n' && n < text_w) {
                char c = *p++;
                if (c == '\t') {
                    c = ' ';
                }
                line[n++] = (c >= 32 && c <= 126) ? c : '.';
            }
            if (*p == '\n') {
                p++;
            }
            line[n] = 0;
            paint_span(api, box_x + 2, box_y + 2 + shown, text_w, C_OUT_FG, C_OUT_BG, ' ');
            paint_text(api, box_x + 2, box_y + 2 + shown, C_OUT_FG, C_OUT_BG, line, text_w);
            shown++;
        }

        btn_y = box_y + box_h - 3;
        btn_x = box_x + (box_w - (btn_n + 2)) / 2;
        paint_text(api, box_x + 2, box_y + box_h - 2, C_OUT_HINT, C_OUT_BG,
                   "Enter OK   Up/Dn scroll", text_w);
        paint_ch(api, btn_x, btn_y, C_OUT_BR, C_OUT_BG, '[');
        paint_text(api, btn_x + 1, btn_y, C_OUT_BTN_FG, C_OUT_BTN_BG, btn, btn_n);
        paint_ch(api, btn_x + 1 + btn_n, btn_y, C_OUT_BR, C_OUT_BG, ']');
        if (api->hit_clear) {
            api->hit_clear();
        }
        if (api->hit_add) {
            api->hit_add(btn_x, btn_y, btn_n + 2, 1, FOS_HIT_ENTER);
        }

        wait_key(api);
        {
            fos_key_event_t ev = api->read_key();
            if (ev.type == FOS_KEY_ENTER) {
                return;
            }
            if (ev.type == FOS_KEY_CHAR &&
                (ev.ch == ' ' || ev.ch == 'o' || ev.ch == 'O' ||
                 ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27 || ev.ch == '\r')) {
                return;
            }
            if (ev.type == FOS_KEY_HOME) {
                scroll = 0;
            } else if (ev.type == FOS_KEY_END) {
                scroll = max_scroll;
            } else if (ev.type == FOS_KEY_UP && scroll > 0) {
                scroll--;
            } else if (ev.type == FOS_KEY_DOWN && scroll < max_scroll) {
                scroll++;
            } else if (ev.type == FOS_KEY_PAGEUP) {
                scroll -= text_h;
                if (scroll < 0) {
                    scroll = 0;
                }
            } else if (ev.type == FOS_KEY_PAGEDOWN) {
                scroll += text_h;
                if (scroll > max_scroll) {
                    scroll = max_scroll;
                }
            }
        }
    }
}

static void run_selected_prog(fos_api_t *api) {
    char path[256];
    char saved_cwd[256];
    char cap[8192];
    char title[64];
    int saved_drive;
    int rc;
    int bat;
    size_t n = 0;
    int (*run)(const char *path, const char *args);

    my_strcpy(title, entries[selection].name);
    bat = is_ext_name(title, ".BAT");
    if (my_stricmp(title, "FM.COM") == 0) {
        show_result(api, "Already in FM");
        draw(api);
        return;
    }
    if (my_stricmp(title, "SHELL.COM") == 0) {
        show_result(api, "Use q to return to the shell");
        draw(api);
        return;
    }

    if (bat) {
        run = api->run_bat;
        if (!run) {
            show_result(api, "Need a newer kernel to run .BAT");
            draw(api);
            return;
        }
    } else {
        run = api->run_com;
    }

    join_path(path, sizeof(path), cwd, title);
    saved_drive = api->get_drive ? api->get_drive() : 0;
    my_strcpy(saved_cwd, cwd);

    cap[0] = 0;
    if (api->begin_capture && api->end_capture) {
        api->begin_capture(cap, sizeof(cap));
        rc = run(path, "");
        n = api->end_capture();
    } else {
        rc = run(path, "");
    }
    if (rc != 0) {
        resume_after_com(api, saved_drive, saved_cwd);
        show_result(api, bat ? "Could not run script" : "Could not run program");
        draw(api);
        return;
    }
    resume_after_com(api, saved_drive, saved_cwd);
    if (n > 0 && output_has_body(cap)) {
        show_com_output(api, title, cap);
    }
    draw(api);
}

static void open_selected(fos_api_t *api) {
    if (entry_count == 0) {
        return;
    }
    fm_entry_t *e = &entries[selection];
    char next[256];

    if (e->is_parent) {
        parent_path(cwd, next, sizeof(next));
        if (api->set_cwd(next) != 0) {
            return;
        }
    } else if (e->is_dir) {
        join_path(next, sizeof(next), cwd, e->name);
        if (api->set_cwd(next) != 0) {
            return;
        }
    } else if (!pick_mode && is_runnable_name(e->name)) {
        run_selected_prog(api);
        return;
    } else if (!pick_mode && is_ext_name(e->name, ".PNT")) {
        char path[256];
        char args[256];
        char saved_cwd[256];
        int saved_drive;

        join_path(path, sizeof(path), cwd, e->name);
        if (path[0] == '\\') {
            my_strcpy(args, path + 1);
        } else {
            my_strcpy(args, path);
        }
        saved_drive = api->get_drive ? api->get_drive() : 0;
        my_strcpy(saved_cwd, cwd);
        api->run_com("\\FOS\\PAINT.COM", args);
        resume_after_com(api, saved_drive, saved_cwd);
        draw(api);
        return;
    } else {
        return;
    }
    reload_entries(api);
}

static uint64_t fm_click_ms;
static int fm_click_sel = -1;

static int handle_fm_mouse(fos_api_t *api) {
    fos_mouse_t m;

    if (!api->mouse_poll || !api->mouse_poll(&m) || !(m.pending & 1)) {
        return 0;
    }
    if (m.x >= list_x && m.x < list_x + list_w &&
        m.y >= list_y && m.y < list_y + list_rows) {
        int idx = list_scroll + (m.y - list_y);
        if (idx >= 0 && idx < entry_count) {
            if (idx == selection && fm_click_sel == idx && api->get_ticks_ms) {
                uint64_t now = api->get_ticks_ms();
                if (now - fm_click_ms < 400ull) {
                    fm_click_sel = -1;
                    open_selected(api);
                    draw(api);
                    return 1;
                }
            }
            selection = idx;
            fm_click_sel = idx;
            if (api->get_ticks_ms) {
                fm_click_ms = api->get_ticks_ms();
            }
            draw(api);
            return 1;
        }
    }
    return 0;
}

static void edit_selected(fos_api_t *api) {
    char path[256];
    char args[256];
    char saved_cwd[256];
    int saved_drive;

    if (entry_count == 0 || entries[selection].is_dir) {
        return;
    }
    join_path(path, sizeof(path), cwd, entries[selection].name);
    if (path[0] == '\\') {
        my_strcpy(args, path + 1);
    } else {
        my_strcpy(args, path);
    }
    saved_drive = api->get_drive ? api->get_drive() : 0;
    my_strcpy(saved_cwd, cwd);
    api->run_com("\\FOS\\EDIT.COM", args);
    resume_after_com(api, saved_drive, saved_cwd);
    draw(api);
}

static void view_selected(fos_api_t *api) {
    char path[256];

    if (entry_count == 0 || entries[selection].is_dir) {
        return;
    }
    join_path(path, sizeof(path), cwd, entries[selection].name);
    view_file(api, path, entries[selection].name);
    draw(api);
}

static void new_dir(fos_api_t *api) {
    char name[64];
    char path[256];

    if (!api->mkdir) {
        draw(api);
        return;
    }
    if (prompt_name(api, "New directory", name, sizeof(name)) != 0) {
        draw(api);
        return;
    }
    join_path(path, sizeof(path), cwd, name);
    if (api->mkdir(path) != 0) {
        draw(api);
        return;
    }
    reload_entries(api);
    draw(api);
}

static void new_file(fos_api_t *api) {
    char name[64];
    char path[256];

    if (prompt_name(api, "New empty file", name, sizeof(name)) != 0) {
        draw(api);
        return;
    }
    join_path(path, sizeof(path), cwd, name);
    {
        int fd = api->fopen(path, FOS_O_WRITE | FOS_O_CREATE | FOS_O_TRUNC);
        if (fd < 0) {
            draw(api);
            return;
        }
        api->fclose(fd);
    }
    reload_entries(api);
    draw(api);
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    const char *start = api->cmdline;

    init_geometry(api);

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start) {
        char path[256];
        if (start[0] >= '0' && start[0] <= '9' && start[1] == ':') {
            my_strcpy(path, start);
        } else if (start[0] == '\\') {
            my_strcpy(path, start);
        } else {
            path[0] = '\\';
            path[1] = 0;
            size_t n = 1;
            for (const char *p = start; *p && n + 1 < sizeof(path); p++) {
                path[n++] = *p;
            }
            path[n] = 0;
        }
        if (api->set_cwd(path) != 0) {
            if (api->show_error) {
                api->show_error("FM: bad path");
            } else {
                api->write_line("FM: bad path");
            }
            return;
        }
    }

    if (reload_entries(api) != 0) {
        if (api->show_error) {
            api->show_error("FM: cannot read directory");
        } else {
            api->write_line("FM: cannot read directory");
        }
        return;
    }

    draw(api);

    for (;;) {
        (void)api->has_key();
        if (handle_fm_mouse(api)) {
            continue;
        }
        if (!api->has_key()) {
            __asm__ volatile("pause");
            continue;
        }

        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }

        if (ev.type == FOS_KEY_CHAR) {
            if (ev.ch == 27) {
                if (pick_mode) {
                    cancel_pick(api);
                    continue;
                }
                break;
            }
            if (ev.ch == 'q' || ev.ch == 'Q') {
                break;
            }
            if (pick_mode && (ev.ch == ' ' || ev.ch == 's' || ev.ch == 'S')) {
                drop_here(api);
                continue;
            }
            if (ev.ch >= '0' && ev.ch <= '9') {
                switch_drive(api, ev.ch - '0');
                continue;
            }
            if (pick_mode) {
                continue;
            }
            if (ev.ch == 'e' || ev.ch == 'E') {
                edit_selected(api);
                continue;
            }
            if (ev.ch == 'v' || ev.ch == 'V') {
                view_selected(api);
                continue;
            }
            if (ev.ch == 'n' || ev.ch == 'N') {
                new_file(api);
                continue;
            }
            if (ev.ch == 'm' || ev.ch == 'M' || ev.ch == 'k' || ev.ch == 'K') {
                new_dir(api);
                continue;
            }
            if (ev.ch == 'c' || ev.ch == 'C') {
                start_pick(api, 0);
                continue;
            }
            if (ev.ch == 'r' || ev.ch == 'R') {
                start_pick(api, 1);
                continue;
            }
            if (ev.ch == 'd' || ev.ch == 'D') {
                delete_selected(api);
                continue;
            }
            continue;
        }

        if (ev.type == FOS_KEY_ENTER) {
            open_selected(api);
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_TAB) {
            int n = api->drive_count ? api->drive_count() : 0;
            int d = api->get_drive ? api->get_drive() : 0;
            if (n > 1) {
                switch_drive(api, (d + 1) % n);
            }
            continue;
        }

        if (ev.type == FOS_KEY_UP && selection > 0) {
            selection--;
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_DOWN && selection + 1 < entry_count) {
            selection++;
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_PAGEUP) {
            selection -= list_rows;
            if (selection < 0) {
                selection = 0;
            }
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_PAGEDOWN) {
            selection += list_rows;
            if (selection >= entry_count) {
                selection = entry_count > 0 ? entry_count - 1 : 0;
            }
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_HOME && entry_count > 0) {
            selection = 0;
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_END && entry_count > 0) {
            selection = entry_count - 1;
            draw(api);
            continue;
        }
    }

    if (api->set_color) {
        api->set_color(15, 0);
    }
    api->clear_screen();
    api->putchar('\n');
}
