/*
 * fm.c — tiny ncurses directory browser
 *
 * What it does:
 *   - Lists subdirectories and regular files in the selected directory
 *     (defaults to current directory, or argv[1] when provided).
 *   - First row can be "[..]" to go to the parent (hidden when cwd is "/").
 *   - Arrow keys move the highlight; PgUp/PgDn move by one screen of names;
 *     Enter opens a directory; c copies or m moves the selected file
 *     (destination picker then filename dialog); d deletes the selected
 *     regular file after a confirmation dialog; v views a regular file in a
 *     full-screen buffer; e opens a regular file in $VISUAL or $EDITOR (else
 *     vi); n creates an empty file (filename dialog); k creates a new
 *     subdirectory (filename dialog); q or F10 quits.
 *
 * Layout (ncurses):
 *   - stdscr: outer frame + background (blue when cwd is writable, red if
 *     the process lacks W_OK on the current directory).
 *   - whead: bordered box with path + key hints.
 *   - wlist: bordered box with the scrollable directory list.
 *
 * Memory:
 *   - Entries (name + is_dir) live in a heap array; free_entries() releases
 *     them whenever we leave a directory or exit.
 *
 * Feature test macro: POSIX strdup and snprintf behavior.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Max file size read into memory for the viewer (see view_file). */
#define VIEW_MAX_BYTES ((size_t)4 * 1024 * 1024)

/* One row in the main browser: directory or regular file. */
typedef struct {
    char *name;
    int is_dir;
} Entry;

/*
 * Color pair IDs (first argument to init_pair / COLOR_PAIR).
 * ncurses reserves pair 0; user pairs start at 1 — we keep that convention.
 */
enum {
    C_BG = 1,   /* spaces / default text: white on blue */
    C_BR,       /* box() borders: cyan on blue */
    C_TITLE,    /* path + list title: yellow on blue */
    C_HELP,     /* hint line: cyan on blue (+ A_DIM when drawing) */
    C_SEL,      /* highlighted list row: black on cyan */
    C_PARENT,   /* "[..]" row when not selected: green on blue */
    /* Same roles when cwd is not writable (access W_OK fails); red theme. */
    C_BG_RO,
    C_BR_RO,
    C_TITLE_RO,
    C_HELP_RO,
    C_SEL_RO,
    C_PARENT_RO,
    C_FILE,    /* regular file rows (main list, writable cwd) */
    C_FILE_RO  /* regular file rows on read-only cwd */
};

/* Enable color pairs if the terminal supports it; no-op otherwise. */
static void init_ui_colors(void) {
    if (!has_colors()) {
        return;
    }
    start_color();
    /*
     * Tune the palette when supported (ncurses RGB scale 0–1000): darker
     * blue background, slightly brighter foreground hues for borders, titles,
     * help, directories, and file names. No-op if the terminal cannot redefine
     * colors.
     */
    if (can_change_color()) {
        init_color(COLOR_BLUE, 0, 0, 520);
        init_color(COLOR_WHITE, 1000, 1000, 1000);
        init_color(COLOR_CYAN, 400, 1000, 1000);
        init_color(COLOR_YELLOW, 1000, 1000, 650);
        init_color(COLOR_GREEN, 450, 1000, 500);
        init_color(COLOR_MAGENTA, 1000, 550, 1000);
    }
    init_pair(C_BG, COLOR_WHITE, COLOR_BLUE);
    init_pair(C_BR, COLOR_CYAN, COLOR_BLUE);
    init_pair(C_TITLE, COLOR_YELLOW, COLOR_BLUE);
    init_pair(C_HELP, COLOR_CYAN, COLOR_BLUE);
    init_pair(C_SEL, COLOR_BLACK, COLOR_CYAN);
    init_pair(C_PARENT, COLOR_GREEN, COLOR_BLUE);
    init_pair(C_BG_RO, COLOR_WHITE, COLOR_RED);
    init_pair(C_BR_RO, COLOR_YELLOW, COLOR_RED);
    init_pair(C_TITLE_RO, COLOR_YELLOW, COLOR_RED);
    init_pair(C_HELP_RO, COLOR_WHITE, COLOR_RED);
    init_pair(C_SEL_RO, COLOR_BLACK, COLOR_WHITE);
    init_pair(C_PARENT_RO, COLOR_GREEN, COLOR_RED);
    init_pair(C_FILE, COLOR_MAGENTA, COLOR_BLUE);
    init_pair(C_FILE_RO, COLOR_MAGENTA, COLOR_RED);
}

/*
 * True if base/name exists and is a directory.
 * stat() follows symlinks; a symlink to a directory counts as a directory.
 */
/* Nonzero if the process may write the directory (create/delete names). */
static int cwd_writable(const char *cwd) {
    return access(cwd, W_OK) == 0;
}

static int is_dir(const char *base, const char *name) {
    char path[PATH_MAX];
    struct stat st;

    if (snprintf(path, sizeof path, "%s/%s", base, name) >= (int)sizeof path)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}

/* qsort() comparator: compare two char* pointers as strings. */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Free every strdup'd name plus the pointer array itself. */
static void free_list(char **names, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

static void free_entries(Entry *e, size_t n) {
    for (size_t i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

static int is_reg_file(const char *base, const char *name) {
    char path[PATH_MAX];
    struct stat st;

    if (snprintf(path, sizeof path, "%s/%s", base, name) >= (int)sizeof path)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISREG(st.st_mode);
}

static int cmp_entries(const void *a, const void *b) {
    const Entry *A = a;
    const Entry *B = b;
    if (A->is_dir != B->is_dir)
        return B->is_dir - A->is_dir; /* directories first */
    return strcmp(A->name, B->name);
}

/*
 * Like collect_dirs but includes regular files; each row knows is_dir.
 * "[..]" is still injected first when not at "/".
 */
static int collect_entries(const char *cwd, Entry **out, size_t *out_n) {
    DIR *d = opendir(cwd);
    if (!d)
        return -1;

    Entry *list = NULL;
    size_t n = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        int isd = is_dir(cwd, ent->d_name);
        if (!isd && !is_reg_file(cwd, ent->d_name))
            continue;

        char *copy = strdup(ent->d_name);
        if (!copy) {
            closedir(d);
            free_entries(list, n);
            return -1;
        }
        Entry *next = realloc(list, (n + 1) * sizeof *next);
        if (!next) {
            free(copy);
            closedir(d);
            free_entries(list, n);
            return -1;
        }
        list = next;
        list[n].name = copy;
        list[n].is_dir = isd;
        n++;
    }
    closedir(d);

    qsort(list, n, sizeof list[0], cmp_entries);

    if (strcmp(cwd, "/") != 0) {
        Entry *next = realloc(list, (n + 1) * sizeof *next);
        if (!next) {
            free_entries(list, n);
            return -1;
        }
        list = next;
        memmove(list + 1, list, n * sizeof *list);
        char *up = strdup("[..]");
        if (!up) {
            memmove(list, list + 1, n * sizeof *list);
            Entry *trim = realloc(list, n * sizeof *trim);
            if (trim)
                list = trim;
            free_entries(list, n);
            return -1;
        }
        list[0].name = up;
        list[0].is_dir = 1;
        n++;
    }

    *out = list;
    *out_n = n;
    return 0;
}

/*
 * Build the list of subdirectory names for cwd.
 *
 * Steps:
 *   1) Read cwd with readdir; skip "." and ".." (we add our own "[..]" later).
 *   2) Keep only names where stat says directory.
 *   3) Sort alphabetically (real dirs only at this point).
 *   4) If not at filesystem root, grow the array, shift everything right by
 *      one slot, and put strdup("[..]") at index 0 so "up" is always on top.
 *
 * Returns 0 on success; on error returns -1 and caller must not use *out_*.
 * On success, *out_names / *out_n must eventually be passed to free_list().
 */
static int collect_dirs(const char *cwd, char ***out_names, size_t *out_n) {
    DIR *d = opendir(cwd);
    if (!d)
        return -1;

    char **names = NULL;
    size_t n = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        /* Hide the real "." and ".." — we inject a labeled "[..]" after sort. */
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        if (!is_dir(cwd, ent->d_name))
            continue;
        char *copy = strdup(ent->d_name);
        if (!copy) {
            closedir(d);
            free_list(names, n);
            return -1;
        }
        char **next = realloc(names, (n + 1) * sizeof *next);
        if (!next) {
            free(copy);
            closedir(d);
            free_list(names, n);
            return -1;
        }
        names = next;
        names[n++] = copy;
    }
    closedir(d);

    qsort(names, n, sizeof names[0], cmp_str);

    /* Parent row: not meaningful at "/" (would be a no-op or wrong). */
    if (strcmp(cwd, "/") != 0) {
        char **next = realloc(names, (n + 1) * sizeof *next);
        if (!next) {
            free_list(names, n);
            return -1;
        }
        names = next;
        /* Make room at index 0: shift old [0..n-1] -> [1..n]. */
        memmove(names + 1, names, n * sizeof *names);
        char *up = strdup("[..]");
        if (!up) {
            /* Undo the shift and bail without leaking duplicate pointers. */
            memmove(names, names + 1, n * sizeof *names);
            char **trim = realloc(names, n * sizeof *names);
            if (trim)
                names = trim;
            free_list(names, n);
            return -1;
        }
        names[0] = up;
        n++;
    }

    *out_names = names;
    *out_n = n;
    return 0;
}

/*
 * Number of directory lines visible at once (same geometry as draw_ui's list
 * interior). Used for PgUp/PgDn step; falls back to 1 if the layout has no
 * inner list rows yet.
 */
static int list_inner_height(int rows, int cols) {
    if (rows < 7 || cols < 12)
        return 1;
    int list_h = rows - 6;
    if (list_h < 1)
        list_h = 1;
    int inner_h = list_h - 2;
    return inner_h < 1 ? 1 : inner_h;
}

/*
 * Full redraw: outer border, header window, list window, then doupdate().
 *
 * rows, cols should match the current stdscr size (updated on KEY_RESIZE).
 *
 * Vertical layout (inside outer box at row 0 / col 0):
 *   row 0        — top border of stdscr
 *   rows 1–4   — whead (4 lines): box + path + help
 *   row 5+     — wlist: directory list with its own box
 *   last row   — bottom border of stdscr
 * Hence list height = rows - 6 (one line gap is absorbed in the constant 5
 * for wlist's y-origin: header uses screen rows 1..4, list starts at row 5).
 *
 * Scrolling: only inner_h lines fit; if sel is below the window, `first` is
 * the index of the top visible row so the selection stays in view.
 *
 * writable: from cwd_writable(cwd); when false, red palette is used.
 * list_title / hint_line: top border label of the list box and header hint.
 */
static void draw_ui(const char *cwd, const Entry *ent, size_t n, size_t sel,
                    int rows, int cols, int writable, const char *list_title,
                    const char *hint_line) {
    /*
     * Active ncurses pairs for this frame (blue theme vs red read-only).
     * Names avoid clashing with `sel` (selected row index).
     */
    int p_bg = C_BG, p_br = C_BR, p_title = C_TITLE, p_help = C_HELP;
    int p_rowsel = C_SEL, p_parent = C_PARENT, p_file = C_FILE;

    if (has_colors() && !writable) {
        p_bg = C_BG_RO;
        p_br = C_BR_RO;
        p_title = C_TITLE_RO;
        p_help = C_HELP_RO;
        p_rowsel = C_SEL_RO;
        p_parent = C_PARENT_RO;
        p_file = C_FILE_RO;
    }

    werase(stdscr);
    if (has_colors()) {
        /* Full-window background for empty margin cells. */
        wbkgd(stdscr, (chtype)' ' | COLOR_PAIR(p_bg));
    }

    if (rows < 7 || cols < 12) {
        mvprintw(0, 0, "Terminal too small (need 7x12).");
        return;
    }

    /* Outer frame on the root window. */
    if (has_colors()) {
        wattron(stdscr, COLOR_PAIR(p_br));
    }
    box(stdscr, 0, 0);
    if (has_colors()) {
        wattroff(stdscr, COLOR_PAIR(p_br));
    }
    wnoutrefresh(stdscr);

    /*
     * Header panel: width cols-2, height 4, at (1,1) so it sits inside the
     * outer box (which uses row 0 and column 0 for its corners/lines).
     */
    WINDOW *whead = newwin(4, cols - 2, 1, 1);
    WINDOW *wlist = NULL;
    if (!whead)
        return;

    if (has_colors()) {
        wbkgd(whead, (chtype)' ' | COLOR_PAIR(p_bg));
        wattron(whead, COLOR_PAIR(p_br));
    }
    box(whead, 0, 0);
    if (has_colors()) {
        wattroff(whead, COLOR_PAIR(p_br));
        wattron(whead, COLOR_PAIR(p_title));
    }
    /* Inner width minus left/right border cells. */
    int hw = getmaxx(whead) - 2;
    if (hw < 1)
        hw = 1;
    /* "Path: " is 6 characters — remaining width is for the path string. */
    mvwprintw(whead, 1, 1, "Path: %.*s", hw - 6 > 0 ? hw - 6 : 0, cwd);
    if (has_colors()) {
        wattroff(whead, COLOR_PAIR(p_title));
        wattron(whead, COLOR_PAIR(p_help) | A_DIM);
    }
    mvwprintw(whead, 2, 1, "%.*s", hw, hint_line);
    if (has_colors()) {
        wattroff(whead, COLOR_PAIR(p_help) | A_DIM);
    }
    wnoutrefresh(whead);

    /* List panel starts after the 4-line header (screen row 5 = index 5). */
    int list_h = rows - 6;
    if (list_h < 1)
        list_h = 1;
    wlist = newwin(list_h, cols - 2, 5, 1);
    if (!wlist) {
        delwin(whead);
        return;
    }
    if (has_colors()) {
        wbkgd(wlist, (chtype)' ' | COLOR_PAIR(p_bg));
        wattron(wlist, COLOR_PAIR(p_br));
    }
    box(wlist, 0, 0);
    if (has_colors()) {
        wattroff(wlist, COLOR_PAIR(p_br));
        wattron(wlist, COLOR_PAIR(p_title));
    }
    /* Label printed on the top border line (common ncurses idiom). */
    mvwprintw(wlist, 0, 2, "%s", list_title);
    if (has_colors()) {
        wattroff(wlist, COLOR_PAIR(p_title));
    }

    /* Usable list area excludes the one-cell border on each side. */
    int inner_h = getmaxy(wlist) - 2;
    int inner_w = getmaxx(wlist) - 2;
    if (inner_h < 1 || inner_w < 1) {
        wnoutrefresh(wlist);
        delwin(wlist);
        delwin(whead);
        doupdate();
        return;
    }

    if (n == 0) {
        if (has_colors()) {
            wattron(wlist, COLOR_PAIR(p_help));
        }
        mvwprintw(wlist, 1, 1, "(nothing here)");
        if (has_colors()) {
            wattroff(wlist, COLOR_PAIR(p_help));
        }
        wnoutrefresh(wlist);
        delwin(wlist);
        delwin(whead);
        doupdate();
        return;
    }

    /* Window scroll: keep the cursor row visible when sel moves down. */
    size_t first = 0;
    if (sel >= (size_t)inner_h) {
        first = sel - (size_t)inner_h + 1;
    }

    for (int i = 0; i < inner_h; i++) {
        size_t idx = first + (size_t)i;
        if (idx >= n)
            break;
        if (idx == sel) {
            if (has_colors()) {
                wattron(wlist, COLOR_PAIR(p_rowsel));
            } else {
                wattron(wlist, A_REVERSE);
            }
        } else if (has_colors() && ent[idx].is_dir &&
                   strcmp(ent[idx].name, "[..]") == 0) {
            wattron(wlist, COLOR_PAIR(p_parent));
        } else if (has_colors() && !ent[idx].is_dir) {
            wattron(wlist, COLOR_PAIR(p_file));
        }
        /* %.*s avoids writing past the right inner border. */
        mvwprintw(wlist, 1 + i, 1, "%.*s", inner_w, ent[idx].name);
        if (idx == sel) {
            if (has_colors()) {
                wattroff(wlist, COLOR_PAIR(p_rowsel));
            } else {
                wattroff(wlist, A_REVERSE);
            }
        } else if (has_colors() && ent[idx].is_dir &&
                   strcmp(ent[idx].name, "[..]") == 0) {
            wattroff(wlist, COLOR_PAIR(p_parent));
        } else if (has_colors() && !ent[idx].is_dir) {
            wattroff(wlist, COLOR_PAIR(p_file));
        }
    }

    wnoutrefresh(wlist);
    delwin(wlist);
    delwin(whead);
    /*
     * doupdate() flushes all wnoutrefresh()d windows in one go; avoids
     * tearing when subwindows overlap the logical layout.
     */
    doupdate();
}

static void draw_transfer_progress(const char *op, off_t done, off_t total) {
    if (!stdscr || isendwin())
        return;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows <= 0 || cols < 24)
        return;

    int bar_w = cols - 34;
    if (bar_w < 10)
        bar_w = 10;
    if (bar_w > 50)
        bar_w = 50;

    double ratio = 0.0;
    if (total > 0)
        ratio = (double)done / (double)total;
    if (ratio < 0.0)
        ratio = 0.0;
    if (ratio > 1.0)
        ratio = 1.0;

    int fill = (int)(ratio * (double)bar_w + 0.5);
    int pct = (int)(ratio * 100.0 + 0.5);

    if (!op)
        op = "Copy";
    if (move(rows - 1, 0) == ERR)
        return;
    if (clrtoeol() == ERR)
        return;
    if (printw("%s [", op) == ERR)
        return;
    for (int i = 0; i < bar_w; i++)
        if (addch(i < fill ? '#' : '-') == ERR)
            return;
    if (total > 0) {
        if (printw("] %3d%% %lld/%lld KiB", pct, (long long)(done / 1024),
                   (long long)(total / 1024)) == ERR)
            return;
    } else {
        if (printw("] %lld KiB", (long long)(done / 1024)) == ERR)
            return;
    }
    refresh();
}

static void clear_transfer_progress(void) {
    if (!stdscr || isendwin())
        return;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    if (rows <= 0)
        return;
    if (move(rows - 1, 0) == ERR)
        return;
    if (clrtoeol() == ERR)
        return;
    refresh();
}

/* Stream copy for regular files. Removes partial dst on failure. */
static int copy_file_bin(const char *src, const char *dst, const char *op) {
    FILE *in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t nr;
    off_t copied = 0;
    off_t total = -1;
    struct stat st;
    if (stat(src, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0)
        total = st.st_size;
    draw_transfer_progress(op, 0, total);
    while ((nr = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, nr, out) != nr) {
            fclose(in);
            fclose(out);
            unlink(dst);
            clear_transfer_progress();
            return -1;
        }
        copied += (off_t)nr;
        draw_transfer_progress(op, copied, total);
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        unlink(dst);
        clear_transfer_progress();
        return -1;
    }
    fclose(in);
    if (fclose(out) != 0) {
        unlink(dst);
        clear_transfer_progress();
        return -1;
    }
    clear_transfer_progress();
    return 0;
}

static int copy_path(const char *src, const char *dst, const char *op);
static int remove_tree(const char *path);

static int copy_dir_recursive(const char *src, const char *dst, const char *op) {
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    if (mkdir(dst, st.st_mode & 0777) != 0)
        return -1;

    DIR *d = opendir(src);
    if (!d) {
        rmdir(dst);
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char src_child[PATH_MAX];
        char dst_child[PATH_MAX];
        if (snprintf(src_child, sizeof src_child, "%s/%s", src, ent->d_name) >=
                (int)sizeof src_child ||
            snprintf(dst_child, sizeof dst_child, "%s/%s", dst, ent->d_name) >=
                (int)sizeof dst_child) {
            closedir(d);
            remove_tree(dst);
            return -1;
        }
        if (copy_path(src_child, dst_child, op) != 0) {
            closedir(d);
            remove_tree(dst);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

static int copy_path(const char *src, const char *dst, const char *op) {
    struct stat st;
    if (stat(src, &st) != 0)
        return -1;
    if (S_ISREG(st.st_mode))
        return copy_file_bin(src, dst, op);
    if (S_ISDIR(st.st_mode))
        return copy_dir_recursive(src, dst, op);
    return -1;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return -1;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char child[PATH_MAX];
            if (snprintf(child, sizeof child, "%s/%s", path, ent->d_name) >=
                (int)sizeof child) {
                closedir(d);
                return -1;
            }
            if (remove_tree(child) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

/*
 * Move a regular file: rename when possible; if cross-device (EXDEV),
 * copy then unlink the source. Mirrors copy_file_bin cleanup on copy failure.
 */
static int move_file(const char *src, const char *dst) {
    if (rename(src, dst) == 0)
        return 0;
    if (errno != EXDEV)
        return -1;
    if (copy_path(src, dst, "Move") != 0)
        return -1;
    if (remove_tree(src) != 0)
        return -1;
    return 0;
}

/* Split buf[0..sz) into newline-separated lines; records start index of each. */
static size_t build_line_starts(const char *buf, size_t sz, size_t **out_starts) {
    size_t n = 0, cap = 256;
    size_t *st = malloc(cap * sizeof *st);
    if (!st) {
        *out_starts = NULL;
        return 0;
    }
    size_t a = 0;
    for (size_t i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            if (n >= cap) {
                cap *= 2;
                size_t *p = realloc(st, cap * sizeof *p);
                if (!p) {
                    free(st);
                    *out_starts = NULL;
                    return 0;
                }
                st = p;
            }
            st[n++] = a;
            a = i + 1;
        }
    }
    *out_starts = st;
    return n;
}

static size_t line_byte_len(size_t sz, const size_t *starts, size_t nlines,
                            size_t k) {
    if (k >= nlines)
        return 0;
    if (k + 1 < nlines)
        return starts[k + 1] - 1U - starts[k];
    return sz - starts[k];
}

static void view_notify(const char *msg) {
    int my, mx;
    getmaxyx(stdscr, my, mx);
    int mw = (int)strlen(msg) + 8;
    if (mw > mx - 2)
        mw = mx - 2;
    if (mw < 24)
        mw = mx - 2 > 12 ? mx - 2 : 12;
    int mh = 5;
    int sy = (my - mh) / 2;
    int sx = (mx - mw) / 2;
    if (sy < 0)
        sy = 0;
    if (sx < 0)
        sx = 0;

    WINDOW *d = newwin(mh, mw, sy, sx);
    if (!d)
        return;
    keypad(d, TRUE);
    if (has_colors())
        wbkgd(d, (chtype)' ' | COLOR_PAIR(C_BG));
    werase(d);
    if (has_colors())
        wattron(d, COLOR_PAIR(C_BR));
    box(d, 0, 0);
    if (has_colors())
        wattroff(d, COLOR_PAIR(C_BR));
    mvwprintw(d, 2, 2, "%s", msg);
    mvwprintw(d, 3, 2, "Any key");
    wrefresh(d);
    wgetch(d);
    delwin(d);
}

/*
 * Full-screen read-only view of a regular file. q / Esc / F10 close.
 * Up/Dn scroll by line; PgUp/PgDn by page; Home/End to ends. Updates
 * *prows / *pcols after KEY_RESIZE. Returns 0 on success, -1 on I/O error.
 */
static int view_file(const char *path, const char *title, int *prows,
                     int *pcols) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        beep();
        return -1;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        beep();
        return -1;
    }
    if (st.st_size < 0 || st.st_size > (off_t)VIEW_MAX_BYTES) {
        fclose(f);
        view_notify("File too large to view.");
        return -1;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        beep();
        return -1;
    }
    if (sz > 0 && fread(buf, 1, sz, f) != sz) {
        free(buf);
        fclose(f);
        beep();
        return -1;
    }
    fclose(f);
    buf[sz] = '\0';

    size_t *starts = NULL;
    size_t nlines = build_line_starts(buf, sz, &starts);
    if (!starts) {
        free(buf);
        beep();
        return -1;
    }

    size_t scroll = 0;
    for (;;) {
        int h, w;
        getmaxyx(stdscr, h, w);
        if (prows && pcols) {
            *prows = h;
            *pcols = w;
        }

        WINDOW *win = newwin(h, w, 0, 0);
        if (!win) {
            free(starts);
            free(buf);
            beep();
            return -1;
        }
        keypad(win, TRUE);
        if (has_colors())
            wbkgd(win, (chtype)' ' | COLOR_PAIR(C_BG));

        int ih = h - 4;
        if (ih < 1)
            ih = 1;
        int inner_w = w - 4;
        if (inner_w < 1)
            inner_w = 1;

        size_t inner_h = (size_t)ih;
        size_t max_scroll =
            nlines > inner_h ? nlines - inner_h : 0;
        if (scroll > max_scroll)
            scroll = max_scroll;

        werase(win);
        if (has_colors())
            wattron(win, COLOR_PAIR(C_BR));
        box(win, 0, 0);
        if (has_colors())
            wattroff(win, COLOR_PAIR(C_BR));
        if (has_colors())
            wattron(win, COLOR_PAIR(C_TITLE));
        mvwprintw(win, 1, 2, "View: %.*s", inner_w > 8 ? inner_w - 8 : 1,
                  title);
        if (has_colors())
            wattroff(win, COLOR_PAIR(C_TITLE));

        for (int r = 0; r < ih; r++) {
            size_t li = scroll + (size_t)r;
            if (li >= nlines) {
                mvwhline(win, 2 + r, 2, ' ', (int)inner_w);
                continue;
            }
            size_t len = line_byte_len(sz, starts, nlines, li);
            const char *p = buf + starts[li];
            while (len > 0 && p[len - 1] == '\r')
                len--;
            int col = 0;
            size_t pos = 0;
            while (pos < len && col < inner_w) {
                unsigned char uc = (unsigned char)p[pos];
                chtype ch = (uc >= 32 && uc < 127) ? (chtype)uc : (chtype)'.';
                mvwaddch(win, 2 + r, 2 + col, ch);
                pos++;
                col++;
            }
            if (col < inner_w)
                mvwhline(win, 2 + r, 2 + col, ' ', inner_w - col);
        }

        if (has_colors())
            wattron(win, COLOR_PAIR(C_HELP));
        mvwprintw(win, h - 2, 2,
                  "q close  Up/Dn  Pg  Home/End");
        if (has_colors())
            wattroff(win, COLOR_PAIR(C_HELP));

        wrefresh(win);
        int c = wgetch(win);
        delwin(win);

        if (c == 'q' || c == 'Q' || c == 27 || c == KEY_F(10)) {
            free(starts);
            free(buf);
            return 0;
        }
        if (c == KEY_RESIZE) {
            resize_term(0, 0);
            getmaxyx(stdscr, h, w);
            if (prows && pcols) {
                *prows = h;
                *pcols = w;
            }
            continue;
        }
        if (c == KEY_UP) {
            if (scroll > 0)
                scroll--;
        } else if (c == KEY_DOWN) {
            if (scroll < max_scroll)
                scroll++;
        } else if (c == KEY_NPAGE) {
            scroll += inner_h;
            if (scroll > max_scroll)
                scroll = max_scroll;
        } else if (c == KEY_PPAGE) {
            if (scroll >= inner_h)
                scroll -= inner_h;
            else
                scroll = 0;
        } else if (c == KEY_HOME) {
            scroll = 0;
        } else if (c == KEY_END) {
            scroll = max_scroll;
        } else
            beep();
    }
}

/*
 * Red confirmation modal for destructive delete. Returns 1 if the user
 * confirms (y/Y), 0 on cancel (n/N, Esc).
 */
static int dialog_confirm_delete(const char *name) {
    int my, mx;
    getmaxyx(stdscr, my, mx);
    int w = mx - 4;
    if (w > 72)
        w = 72;
    if (w < 28)
        w = mx - 4 > 20 ? mx - 4 : 20;
    int h = 7;
    int sy = (my - h) / 2;
    int sx = (mx - w) / 2;
    if (sy < 1)
        sy = 1;
    if (sx < 1)
        sx = 1;

    WINDOW *dlg = newwin(h, w, sy, sx);
    if (!dlg)
        return 0;
    keypad(dlg, TRUE);
    if (has_colors())
        wbkgd(dlg, (chtype)' ' | COLOR_PAIR(C_BG_RO));

    curs_set(0);

    for (;;) {
        werase(dlg);
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_BR_RO));
        box(dlg, 0, 0);
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_BR_RO));
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_TITLE_RO));
        mvwprintw(dlg, 1, 2, "Delete this file?");
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_TITLE_RO));
        int maxshow = w - 4;
        if (maxshow < 1)
            maxshow = 1;
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_HELP_RO));
        mvwprintw(dlg, 3, 2, "%.*s", maxshow, name);
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_HELP_RO));
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_HELP_RO));
        mvwprintw(dlg, h - 2, 2, "y delete  n cancel");
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_HELP_RO));
        wrefresh(dlg);

        int c = wgetch(dlg);
        if (c == 'y' || c == 'Y') {
            delwin(dlg);
            return 1;
        }
        if (c == 'n' || c == 'N' || c == 27) {
            delwin(dlg);
            return 0;
        }
        beep();
    }
}

/*
 * Modal centered editor for a single path segment (no '/').
 * `title` is shown on the first line inside the box (truncated to fit).
 * Returns 0 and fills `out` (NUL-terminated), or -1 on Esc cancel.
 */
static int dialog_filename(const char *title, const char *suggest, char *out,
                           size_t outsz) {
    char buf[NAME_MAX];
    snprintf(buf, sizeof buf, "%s", suggest);
    size_t len = strlen(buf);
    size_t cur = len;

    int my, mx;
    getmaxyx(stdscr, my, mx);
    int w = mx - 4;
    if (w > 64)
        w = 64;
    if (w < 24)
        w = mx - 4 > 12 ? mx - 4 : 12;
    int h = 9;
    int sy = (my - h) / 2;
    int sx = (mx - w) / 2;
    if (sy < 1)
        sy = 1;
    if (sx < 1)
        sx = 1;

    WINDOW *dlg = newwin(h, w, sy, sx);
    if (!dlg)
        return -1;
    keypad(dlg, TRUE);
    if (has_colors())
        wbkgd(dlg, (chtype)' ' | COLOR_PAIR(C_BG));

    curs_set(1);

    for (;;) {
        werase(dlg);
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_BR));
        box(dlg, 0, 0);
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_BR));
        mvwprintw(dlg, 1, 2, "%.*s", w - 4, title);
        mvwprintw(dlg, 2, 2, "Esc cancel  Enter confirm");
        int maxshow = w - 4;
        if (maxshow < 1)
            maxshow = 1;
        size_t disp0 = 0;
        if ((int)len > maxshow)
            disp0 = len - (size_t)maxshow;
        if (has_colors())
            wattron(dlg, COLOR_PAIR(C_TITLE));
        mvwhline(dlg, 4, 1, ' ', w - 2);
        mvwprintw(dlg, 4, 2, "%.*s", maxshow, buf + disp0);
        if (has_colors())
            wattroff(dlg, COLOR_PAIR(C_TITLE));
        {
            int col = 2 + (int)(cur - disp0);
            if (col < 2)
                col = 2;
            if (col > w - 2)
                col = w - 2;
            wmove(dlg, 4, col);
        }
        wrefresh(dlg);

        int c = wgetch(dlg);
        if (c == 27) {
            curs_set(0);
            delwin(dlg);
            return -1;
        }
        if (c == '\n' || c == KEY_ENTER) {
            if (len == 0 || strchr(buf, '/') != NULL) {
                beep();
                continue;
            }
            if (strcmp(buf, ".") == 0 || strcmp(buf, "..") == 0) {
                beep();
                continue;
            }
            snprintf(out, outsz, "%s", buf);
            curs_set(0);
            delwin(dlg);
            return 0;
        }
        if (c == KEY_BACKSPACE || c == 127 || c == '\b') {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur + 1);
                cur--;
                len--;
            }
            continue;
        }
        if (c == KEY_DC) {
            if (cur < len) {
                memmove(buf + cur, buf + cur + 1, len - cur);
                len--;
            }
            continue;
        }
        if (c == KEY_LEFT) {
            if (cur > 0)
                cur--;
            continue;
        }
        if (c == KEY_RIGHT) {
            if (cur < len)
                cur++;
            continue;
        }
        if (c == KEY_HOME) {
            cur = 0;
            continue;
        }
        if (c == KEY_END) {
            cur = len;
            continue;
        }
        if ((unsigned char)c >= ' ' && c < 127 && len + 1 < sizeof buf) {
            if (c == '/')
                continue;
            memmove(buf + cur + 1, buf + cur, len - cur + 1);
            buf[cur] = (char)c;
            cur++;
            len++;
        }
    }
}

/*
 * Sub-browser: directories only. Starts in current getcwd().
 * 's' stores that directory into out_dir and returns 0 (cwd restored).
 * 'q' or F10 cancels (-1). Enter opens a subdirectory.
 * On failure after chdir, restores start cwd before returning -1.
 */
static int pick_destination(char *out_dir, int *rows, int *cols) {
    char start[PATH_MAX];
    if (!getcwd(start, sizeof start))
        return -1;

    char **dnames = NULL;
    size_t dn = 0;
    size_t dsel = 0;
    char dcwd[PATH_MAX];
    if (!getcwd(dcwd, sizeof dcwd))
        return -1;
    if (collect_dirs(dcwd, &dnames, &dn) != 0)
        return -1;

    for (;;) {
        Entry *ev = NULL;
        if (dn > 0) {
            ev = malloc(dn * sizeof *ev);
            if (!ev) {
                beep();
                chdir(start);
                free_list(dnames, dn);
                return -1;
            }
            for (size_t i = 0; i < dn; i++) {
                ev[i].name = dnames[i];
                ev[i].is_dir = 1;
            }
        }
        draw_ui(dcwd, ev, dn, dsel, *rows, *cols, cwd_writable(dcwd),
                " Destination ", "Enter dir  s use here  q/F10 cancel");
        free(ev);

        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == KEY_F(10)) {
            chdir(start);
            free_list(dnames, dn);
            return -1;
        }
        if (ch == 's' || ch == 'S') {
            if (!getcwd(out_dir, PATH_MAX)) {
                chdir(start);
                free_list(dnames, dn);
                return -1;
            }
            chdir(start);
            free_list(dnames, dn);
            return 0;
        }
        if (ch == KEY_RESIZE) {
            resize_term(0, 0);
            getmaxyx(stdscr, *rows, *cols);
            continue;
        }
        if (dn == 0)
            continue;

        if (ch == KEY_UP) {
            if (dsel > 0)
                dsel--;
        } else if (ch == KEY_DOWN) {
            if (dsel + 1 < dn)
                dsel++;
        } else if (ch == KEY_NPAGE) {
            int step = list_inner_height(*rows, *cols);
            size_t next = dsel + (size_t)step;
            dsel = next < dn ? next : dn - 1;
        } else if (ch == KEY_PPAGE) {
            int step = list_inner_height(*rows, *cols);
            if (dsel >= (size_t)step)
                dsel -= (size_t)step;
            else
                dsel = 0;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            const char *pick = dnames[dsel];
            char nextpath[PATH_MAX];

            if (strcmp(pick, "[..]") == 0) {
                if (chdir("..") != 0)
                    beep();
                else {
                    if (!getcwd(dcwd, sizeof dcwd)) {
                        chdir(start);
                        free_list(dnames, dn);
                        return -1;
                    }
                    free_list(dnames, dn);
                    dnames = NULL;
                    dn = 0;
                    if (collect_dirs(dcwd, &dnames, &dn) != 0) {
                        chdir(start);
                        return -1;
                    }
                    dsel = 0;
                }
            } else {
                if (snprintf(nextpath, sizeof nextpath, "%s/%s", dcwd, pick) >=
                    (int)sizeof nextpath) {
                    beep();
                    continue;
                }
                if (chdir(nextpath) != 0)
                    beep();
                else {
                    if (!getcwd(dcwd, sizeof dcwd)) {
                        chdir(start);
                        free_list(dnames, dn);
                        return -1;
                    }
                    free_list(dnames, dn);
                    dnames = NULL;
                    dn = 0;
                    if (collect_dirs(dcwd, &dnames, &dn) != 0) {
                        chdir(start);
                        return -1;
                    }
                    dsel = 0;
                }
            }
        }
    }
}

/*
 * Suspend curses and run the user's editor on `path` (POSIX: VISUAL, then
 * EDITOR, otherwise vi). Restores the terminal when the child exits.
 */
static void run_editor_on_path(const char *path) {
    endwin();
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        reset_prog_mode();
        flushinp();
        clearok(stdscr, TRUE);
        refresh();
        beep();
        return;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              "exec ${VISUAL:-${EDITOR:-vi}} \"$1\"", "fm-edit", path,
              (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    (void)status;
    reset_prog_mode();
    flushinp();
    clearok(stdscr, TRUE);
    clear();
    refresh();
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [path]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        struct stat st;
        if (stat(argv[1], &st) != 0) {
            perror(argv[1]);
            return 1;
        }
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s: not a directory\n", argv[1]);
            return 1;
        }
        if (chdir(argv[1]) != 0) {
            perror(argv[1]);
            return 1;
        }
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        perror("getcwd");
        return 1;
    }

    /* Terminal setup: raw-ish line discipline, keypad for arrows, no echo. */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_ui_colors();

    Entry *entries = NULL;
    size_t n = 0;
    size_t sel = 0;

    if (collect_entries(cwd, &entries, &n) != 0) {
        endwin();
        perror(cwd);
        return 1;
    }
    if (n > 0)
        sel = 0;

    /* Cached size; KEY_RESIZE updates these after resize_term(). */
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    for (;;) {
        draw_ui(cwd, entries, n, sel, rows, cols, cwd_writable(cwd),
                " Browser ",
                "Up/Dn Pg  Enter dir  c copy  m move  d del  v view  e edit  n new  k mkdir  q/F10 quit");
        /*
         * draw_ui already pushed updates with doupdate(); refresh() syncs
         * stdscr in case any drawing path touched it without doupdate.
         */
        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == KEY_F(10))
            break;
        if (ch == KEY_RESIZE) {
            /* Tell ncurses the new LINES/COLS from SIGWINCH. */
            resize_term(0, 0);
            getmaxyx(stdscr, rows, cols);
            continue;
        }
        if (ch == 'n' || ch == 'N') {
            if (!cwd_writable(cwd)) {
                beep();
                continue;
            }
            char newname[NAME_MAX];
            if (dialog_filename("New empty file", "", newname, sizeof newname) !=
                0)
                continue;
            char path[PATH_MAX];
            if (snprintf(path, sizeof path, "%s/%s", cwd, newname) >=
                (int)sizeof path) {
                beep();
                continue;
            }
            int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0) {
                beep();
                continue;
            }
            if (close(fd) != 0) {
                unlink(path);
                beep();
                continue;
            }
            free_entries(entries, n);
            entries = NULL;
            n = 0;
            if (collect_entries(cwd, &entries, &n) != 0) {
                endwin();
                perror(cwd);
                return 1;
            }
            sel = 0;
            for (size_t i = 0; i < n; i++) {
                if (strcmp(entries[i].name, newname) == 0) {
                    sel = i;
                    break;
                }
            }
            continue;
        }
        if (ch == 'k' || ch == 'K') {
            if (!cwd_writable(cwd)) {
                beep();
                continue;
            }
            char newname[NAME_MAX];
            if (dialog_filename("New directory", "", newname, sizeof newname) !=
                0)
                continue;
            char path[PATH_MAX];
            if (snprintf(path, sizeof path, "%s/%s", cwd, newname) >=
                (int)sizeof path) {
                beep();
                continue;
            }
            if (mkdir(path, 0755) != 0) {
                beep();
                continue;
            }
            free_entries(entries, n);
            entries = NULL;
            n = 0;
            if (collect_entries(cwd, &entries, &n) != 0) {
                endwin();
                perror(cwd);
                return 1;
            }
            sel = 0;
            for (size_t i = 0; i < n; i++) {
                if (strcmp(entries[i].name, newname) == 0) {
                    sel = i;
                    break;
                }
            }
            continue;
        }
        /* At "/" with no dirs, n==0 — ignore motion keys until q. */
        if (n == 0) {
            if (ch == KEY_UP || ch == KEY_DOWN || ch == KEY_PPAGE ||
                ch == KEY_NPAGE || ch == 'c' || ch == 'C' || ch == 'm' ||
                ch == 'M' || ch == 'd' || ch == 'D' || ch == 'v' ||
                ch == 'V' || ch == 'e' || ch == 'E')
                continue;
            continue;
        }
        if (ch == KEY_UP) {
            if (sel > 0)
                sel--;
        } else if (ch == KEY_DOWN) {
            if (sel + 1 < n)
                sel++;
        } else if (ch == KEY_NPAGE) {
            int step = list_inner_height(rows, cols);
            size_t next = sel + (size_t)step;
            sel = next < n ? next : n - 1;
        } else if (ch == KEY_PPAGE) {
            int step = list_inner_height(rows, cols);
            if (sel >= (size_t)step)
                sel -= (size_t)step;
            else
                sel = 0;
        } else if (ch == 'c' || ch == 'C') {
            const Entry *e = &entries[sel];
            if (strcmp(e->name, "[..]") == 0) {
                beep();
                continue;
            }
            char src[PATH_MAX];
            struct stat st;
            if (snprintf(src, sizeof src, "%s/%s", cwd, e->name) >=
                (int)sizeof src) {
                beep();
                continue;
            }
            if (stat(src, &st) != 0 ||
                (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
                beep();
                continue;
            }
            char destdir[PATH_MAX];
            if (pick_destination(destdir, &rows, &cols) != 0)
                continue;
            char destname[NAME_MAX];
            if (dialog_filename("Destination file name", e->name, destname,
                                 sizeof destname) != 0)
                continue;
            char dstpath[PATH_MAX];
            if (snprintf(dstpath, sizeof dstpath, "%s/%s", destdir,
                         destname) >= (int)sizeof dstpath) {
                beep();
                continue;
            }
            if (copy_path(src, dstpath, "Copy") != 0)
                beep();
        } else if (ch == 'm' || ch == 'M') {
            const Entry *e = &entries[sel];
            if (strcmp(e->name, "[..]") == 0) {
                beep();
                continue;
            }
            char src[PATH_MAX];
            struct stat st;
            if (snprintf(src, sizeof src, "%s/%s", cwd, e->name) >=
                (int)sizeof src) {
                beep();
                continue;
            }
            if (stat(src, &st) != 0 ||
                (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
                beep();
                continue;
            }
            char destdir[PATH_MAX];
            if (pick_destination(destdir, &rows, &cols) != 0)
                continue;
            char destname[NAME_MAX];
            if (dialog_filename("Destination file name", e->name, destname,
                                 sizeof destname) != 0)
                continue;
            char dstpath[PATH_MAX];
            if (snprintf(dstpath, sizeof dstpath, "%s/%s", destdir,
                         destname) >= (int)sizeof dstpath) {
                beep();
                continue;
            }
            if (move_file(src, dstpath) != 0) {
                beep();
            } else {
                free_entries(entries, n);
                entries = NULL;
                n = 0;
                if (collect_entries(cwd, &entries, &n) != 0) {
                    endwin();
                    perror(cwd);
                    return 1;
                }
                if (n == 0)
                    sel = 0;
                else if (sel >= n)
                    sel = n - 1;
            }
        } else if (ch == 'v' || ch == 'V') {
            const Entry *e = &entries[sel];
            if (e->is_dir) {
                beep();
                continue;
            }
            char path[PATH_MAX];
            struct stat st;
            if (snprintf(path, sizeof path, "%s/%s", cwd, e->name) >=
                (int)sizeof path) {
                beep();
                continue;
            }
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                beep();
                continue;
            }
            view_file(path, e->name, &rows, &cols);
        } else if (ch == 'e' || ch == 'E') {
            const Entry *e = &entries[sel];
            if (e->is_dir) {
                beep();
                continue;
            }
            char path[PATH_MAX];
            struct stat st;
            if (snprintf(path, sizeof path, "%s/%s", cwd, e->name) >=
                (int)sizeof path) {
                beep();
                continue;
            }
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
                beep();
                continue;
            }
            run_editor_on_path(path);
        } else if (ch == 'd' || ch == 'D') {
            const Entry *e = &entries[sel];
            if (strcmp(e->name, "[..]") == 0) {
                beep();
                continue;
            }
            char path[PATH_MAX];
            struct stat st;
            if (snprintf(path, sizeof path, "%s/%s", cwd, e->name) >=
                (int)sizeof path) {
                beep();
                continue;
            }
            if (stat(path, &st) != 0 ||
                (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
                beep();
                continue;
            }
            if (!dialog_confirm_delete(e->name))
                continue;
            if (remove_tree(path) != 0) {
                beep();
                continue;
            }
            free_entries(entries, n);
            entries = NULL;
            n = 0;
            if (collect_entries(cwd, &entries, &n) != 0) {
                endwin();
                perror(cwd);
                return 1;
            }
            if (n == 0)
                sel = 0;
            else if (sel >= n)
                sel = n - 1;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            const Entry *pick = &entries[sel];
            char next[PATH_MAX];

            if (strcmp(pick->name, "[..]") == 0) {
                /* Synthetic row: real path is always ".." for chdir. */
                if (chdir("..") != 0)
                    beep();
                else {
                    if (!getcwd(cwd, sizeof cwd)) {
                        endwin();
                        perror("getcwd");
                        free_entries(entries, n);
                        return 1;
                    }
                    free_entries(entries, n);
                    entries = NULL;
                    n = 0;
                    if (collect_entries(cwd, &entries, &n) != 0) {
                        endwin();
                        perror(cwd);
                        return 1;
                    }
                    sel = 0;
                }
            } else if (!pick->is_dir) {
                beep();
            } else {
                /* Enter a child directory by absolute path from cwd + name. */
                if (snprintf(next, sizeof next, "%s/%s", cwd, pick->name) >=
                    (int)sizeof next) {
                    beep();
                    continue;
                }
                if (chdir(next) != 0)
                    beep();
                else {
                    if (!getcwd(cwd, sizeof cwd)) {
                        endwin();
                        perror("getcwd");
                        free_entries(entries, n);
                        return 1;
                    }
                    free_entries(entries, n);
                    entries = NULL;
                    n = 0;
                    if (collect_entries(cwd, &entries, &n) != 0) {
                        endwin();
                        perror(cwd);
                        return 1;
                    }
                    sel = 0;
                }
            }
        }
    }

    free_entries(entries, n);
    endwin();
    return 0;
}
