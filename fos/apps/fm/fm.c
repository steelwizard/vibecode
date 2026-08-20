/*
 * fm.c — FOS port of the directory browser (see ../../fm.c/fm.c).
 *
 * Keys (same spirit as the ncurses original):
 *   Up/Dn/PgUp/PgDn  move selection
 *   Enter            open directory
 *   e                edit file (EDIT.COM)
 *   v                view file
 *   n                new empty file
 *   m                mkdir
 *   q                quit
 */

#include "fos_api.h"

#define COLS       80
#define LIST_ROWS  20
#define ROW_PATH   0
#define ROW_LIST   2
#define ROW_HELP   23

#define ENTRY_MAX  128

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

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
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

static void ensure_visible(void) {
    if (selection < list_scroll) {
        list_scroll = selection;
    }
    if (selection >= list_scroll + LIST_ROWS) {
        list_scroll = selection - LIST_ROWS + 1;
    }
}

static void draw(fos_api_t *api) {
    char line[COLS + 1];

    ensure_visible();
    api->clear_screen();

    api->goto_xy(0, ROW_PATH);
    line[0] = 0;
    {
        const char *prefix = "Path: ";
        size_t n = 0;
        for (const char *p = prefix; *p && n + 1 < COLS; p++) {
            line[n++] = *p;
        }
        for (const char *p = cwd; *p && n + 1 < COLS; p++) {
            line[n++] = *p;
        }
        line[n] = 0;
    }
    api->write(line);

    for (int row = 0; row < LIST_ROWS; row++) {
        int idx = list_scroll + row;
        size_t n;
        api->goto_xy(0, ROW_LIST + row);
        if (idx >= entry_count) {
            api->write("                                                                                ");
            continue;
        }
        n = 0;
        if (idx == selection) {
            line[n++] = '>';
            line[n++] = ' ';
        } else {
            line[n++] = ' ';
            line[n++] = ' ';
        }
        if (entries[idx].is_dir) {
            line[n++] = '[';
        }
        for (const char *p = entries[idx].name; *p && n + 2 < COLS; p++) {
            line[n++] = *p;
        }
        if (entries[idx].is_dir) {
            line[n++] = ']';
        }
        line[n] = 0;
        api->write(line);
    }

    api->goto_xy(0, ROW_HELP);
    api->write("Enter  e edit  v view  n file  m mkdir  q quit");
}

static void view_file(fos_api_t *api, const char *path, const char *title) {
    static char buf[8192];
    size_t len = 0;
    int scroll = 0;
    const int view_rows = 22;

    if (api->read_file(path, buf, sizeof(buf) - 1, &len) != 0) {
        return;
    }
    buf[len] = 0;

    for (;;) {
        int lines = 1;
        for (size_t i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                lines++;
            }
        }

        api->clear_screen();
        api->goto_xy(0, 0);
        api->write("View: ");
        api->write(title);
        api->goto_xy(0, 1);

        int shown = 0;
        int line_no = 0;
        char line[COLS + 1];
        size_t i = 0;
        while (i < len && shown < view_rows) {
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
            size_t n = 0;
            while (i < len && buf[i] != '\n' && n < COLS) {
                char c = buf[i++];
                line[n++] = (c >= 32 && c <= 126) ? c : '.';
            }
            if (i < len && buf[i] == '\n') {
                i++;
            }
            line[n] = 0;
            api->goto_xy(0, 2 + shown);
            api->write(line);
            shown++;
            line_no++;
        }

        api->goto_xy(0, 24);
        api->write("q close  Up/Dn/Pg");

        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        fos_key_event_t ev = api->read_key();
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27)) {
            return;
        }
        if (ev.type == FOS_KEY_UP && scroll > 0) {
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

static int prompt_name(fos_api_t *api, const char *title, char *out, size_t out_sz) {
    char buf[64];
    size_t len = 0;

    for (;;) {
        api->goto_xy(0, ROW_HELP);
        api->write("                                                                                ");
        api->goto_xy(0, ROW_HELP);
        api->write(title);
        api->write(": ");
        api->write(buf);

        while (!api->has_key()) {
            __asm__ volatile("pause");
        }
        fos_key_event_t ev = api->read_key();
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
        if (ev.type == FOS_KEY_BACKSPACE && len > 0) {
            len--;
            buf[len] = 0;
            continue;
        }
        if (ev.type == FOS_KEY_CHAR && ev.ch >= 32 && ev.ch <= 126 && ev.ch != '\\') {
            if (len + 1 >= sizeof(buf) || len + 1 >= out_sz) {
                continue;
            }
            buf[len++] = ev.ch;
            buf[len] = 0;
        }
    }
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
    } else {
        return;
    }
    reload_entries(api);
}

static void edit_selected(fos_api_t *api) {
    char path[256];
    char args[256];

    if (entry_count == 0 || entries[selection].is_dir) {
        return;
    }
    join_path(path, sizeof(path), cwd, entries[selection].name);
    if (path[0] == '\\') {
        my_strcpy(args, path + 1);
    } else {
        my_strcpy(args, path);
    }
    api->run_com("\\EDIT.COM", args);
    reload_entries(api);
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
    if (prompt_name(api, "New folder (Esc cancel)", name, sizeof(name)) != 0) {
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

    if (prompt_name(api, "New file (Esc cancel)", name, sizeof(name)) != 0) {
        draw(api);
        return;
    }
    join_path(path, sizeof(path), cwd, name);
    if (api->write_file(path, "", 0) != 0) {
        draw(api);
        return;
    }
    reload_entries(api);
    draw(api);
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    const char *start = api->cmdline;

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start) {
        char path[256];
        if (start[0] == '\\') {
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
            api->write_line("FM: bad path");
            return;
        }
    }

    if (reload_entries(api) != 0) {
        api->write_line("FM: cannot read directory");
        return;
    }

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
            if (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27) {
                break;
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
            if (ev.ch == 'm' || ev.ch == 'M') {
                new_dir(api);
                continue;
            }
            continue;
        }

        if (ev.type == FOS_KEY_ENTER) {
            open_selected(api);
            draw(api);
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
            selection -= LIST_ROWS;
            if (selection < 0) {
                selection = 0;
            }
            draw(api);
            continue;
        }

        if (ev.type == FOS_KEY_PAGEDOWN) {
            selection += LIST_ROWS;
            if (selection >= entry_count) {
                selection = entry_count > 0 ? entry_count - 1 : 0;
            }
            draw(api);
            continue;
        }
    }

    api->clear_screen();
    api->putchar('\n');
}
