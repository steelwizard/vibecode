/*
 * shell.c — DOS-style shell with .COM programs, pipes (|), and redirect (>).
 */

#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "vfs.h"
#include "block.h"
#include "boot_report.h"
#include "foscom.h"
#include "fos_api.h"
#include "string.h"

#define LINE_MAX       256
#define CAPTURE_MAX    4096
#define MAX_STAGES     8
#define HIST_MAX       32

typedef struct {
    char cmd[LINE_MAX];
    char redirect[VFS_PATH_MAX];
} shell_stage_t;

static char line_buf[LINE_MAX];
static int line_len = 0;

static char hist_buf[HIST_MAX][LINE_MAX];
static int hist_len = 0;
static int hist_pos = -1;
static char hist_draft[LINE_MAX];

static void reboot(void) {
    uint8_t good = 0x02;
    while ((good & 0x02) == 0) {
        __asm__ volatile("inb $0x64, %0" : "=a"(good));
    }
    __asm__ volatile("outb %0, $0x64" : : "a"((uint8_t)0xFE));
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void trim(char *s) {
    size_t n = strlen(s);
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') {
        start++;
    }
    while (n > start && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    if (start > 0) {
        memmove(s, s + start, n - start);
    }
    s[n - start] = 0;
}

static void print_banner(void) {
    console_set_color(11, 0);
    console_write_line("FOS shell");
    console_set_color(7, 0);
    console_write_line("Flash Operating System. Type help for commands.");
    console_write_line("Use | to pipe and > to redirect output.");
    console_write_line("");
}

static void print_prompt(void) {
    char prompt[64];
    vfs_format_prompt(prompt, sizeof(prompt));
    console_set_color(10, 0);
    console_write(prompt);
    console_set_color(15, 0);
}

static int match_cmd(const char *line, const char *cmd) {
    size_t n = strlen(cmd);
    return strncasecmp(line, cmd, n) == 0 && (line[n] == 0 || line[n] == ' ');
}

static const char *cmd_arg(const char *line) {
    line = skip_spaces(line);
    while (*line && *line != ' ') {
        line++;
    }
    return skip_spaces(line);
}

static void split_cmd(const char *line, char *name, size_t name_sz, char *args, size_t args_sz) {
    line = skip_spaces(line);
    size_t ni = 0;
    while (*line && *line != ' ' && ni + 1 < name_sz) {
        name[ni++] = *line++;
    }
    name[ni] = 0;
    while (*line == ' ') {
        line++;
    }
    strncpy(args, line, args_sz - 1);
    args[args_sz - 1] = 0;
}

static void build_com_path(const char *name, char *path, size_t sz) {
    char base[64];
    size_t i = 0;
    const char *n = name;

    if (n[0] == '\\') {
        strncpy(path, n, sz - 1);
        path[sz - 1] = 0;
        return;
    }

    while (*n && i + 1 < sizeof(base)) {
        char c = *n++;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        base[i++] = c;
    }
    base[i] = 0;

    if (!strchr(base, '.')) {
        strncat(base, ".COM", sizeof(base) - strlen(base) - 1);
    }

    path[0] = '\\';
    path[1] = 0;
    strncat(path, base, sz - 2);
}

static void cmd_help(void) {
    console_write_line("Commands (case insensitive):");
    console_write_line("  help                 Command list");
    console_write_line("  ver                  Version");
    console_write_line("  cls / clear          Clear screen");
    console_write_line("  dir [path]           List directory");
    console_write_line("  cd <path>            Change directory");
    console_write_line("  type <path>          Print file (or piped text)");
    console_write_line("  drives / df          List drives");
    console_write_line("  <n>:                 Switch drive");
    console_write_line("  reboot               Reboot");
    console_write_line("  <name>.com [args]    Run .COM program (e.g. echo hello)");
    console_write_line("  cmd1 | cmd2          Pipe stdout to stdin");
    console_write_line("  cmd > file           Redirect stdout to file");
    console_write_line("  Up/Down              Command history");
    console_write_line("  Tab                  Filename completion");
}

static void cmd_ver(void) {
    console_write_line("FOS v0.3 - Flash Operating System");
    console_write_line("FOSCOM .COM programs, pipes, redirect");
}

static void cmd_drives(void) {
    boot_print_ata_disks();
    console_write_line("");
    vfs_print_drive_table();
}

static void cmd_dir(const char *args) {
    if (vfs_list_dir(vfs_get_drive(), args[0] ? args : "\\") != 0) {
        console_write_line("DIR failed");
    }
}

static void cmd_cd(const char *args) {
    if (*args == 0) {
        console_write_line("CD: path required");
        return;
    }
    if (vfs_set_cwd(args) != 0) {
        console_write_line("CD failed");
    }
}

static void cmd_type(const char *args) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;

    if (*args == 0 && api->pipe_in_len > 0) {
        console_write_n(api->pipe_in, api->pipe_in_len);
        if (api->pipe_in[api->pipe_in_len - 1] != '\n') {
            console_putchar('\n');
        }
        return;
    }

    if (*args == 0) {
        console_write_line("TYPE: file required");
        return;
    }

    static char buf[4096];
    size_t len = 0;
    if (vfs_read_file(vfs_get_drive(), args, buf, sizeof(buf), &len) != 0) {
        console_write_line("TYPE failed");
        return;
    }
    console_write(buf);
    if (len == 0 || buf[len - 1] != '\n') {
        console_putchar('\n');
    }
}

static int try_drive_switch(char *line) {
    const char *s = skip_spaces(line);
    if (s[0] >= '0' && s[0] <= '9' && s[1] == ':') {
        int d = s[0] - '0';
        if (vfs_set_drive(d) == 0) {
            return 1;
        }
    }
    return 0;
}

static int try_run_com(const char *line) {
    char name[64];
    char args[256];
    char path[VFS_PATH_MAX];

    split_cmd(line, name, sizeof(name), args, sizeof(args));
    if (name[0] == 0) {
        return 0;
    }

    build_com_path(name, path, sizeof(path));
    return exec_run(vfs_get_drive(), path, args) == 0;
}

static int run_builtin(char *line) {
    if (try_drive_switch(line)) {
        return 1;
    }

    if (match_cmd(line, "HELP") || match_cmd(line, "?")) {
        cmd_help();
    } else if (match_cmd(line, "VER")) {
        cmd_ver();
    } else if (match_cmd(line, "CLS") || match_cmd(line, "CLEAR")) {
        console_clear();
    } else if (match_cmd(line, "DIR")) {
        cmd_dir(cmd_arg(line));
    } else if (match_cmd(line, "CD")) {
        cmd_cd(cmd_arg(line));
    } else if (match_cmd(line, "TYPE")) {
        cmd_type(cmd_arg(line));
    } else if (match_cmd(line, "DRIVES") || match_cmd(line, "DF")) {
        cmd_drives();
    } else if (match_cmd(line, "REBOOT")) {
        console_write_line("Rebooting...");
        reboot();
    } else if (try_run_com(line)) {
        return 1;
    } else {
        console_write("Unknown: ");
        console_write_line(line);
    }
    return 1;
}

static void parse_redirect(shell_stage_t *st) {
    char *gt = 0;
    for (char *p = st->cmd; *p; p++) {
        if (*p == '>') {
            gt = p;
            break;
        }
    }
    st->redirect[0] = 0;
    if (!gt) {
        trim(st->cmd);
        return;
    }

    *gt = 0;
    trim(st->cmd);
    gt++;
    while (*gt == ' ' || *gt == '\t') {
        gt++;
    }
    strncpy(st->redirect, gt, sizeof(st->redirect) - 1);
    st->redirect[sizeof(st->redirect) - 1] = 0;
    trim(st->redirect);
}

static int parse_pipeline(char *line, shell_stage_t *stages, int max) {
    int n = 0;
    char *start = line;

    while (n < max) {
        char *bar = 0;
        for (char *p = start; *p; p++) {
            if (*p == '|') {
                bar = p;
                break;
            }
        }

        if (bar) {
            *bar = 0;
        }

        trim(start);
        if (*start) {
            strncpy(stages[n].cmd, start, sizeof(stages[n].cmd) - 1);
            stages[n].cmd[sizeof(stages[n].cmd) - 1] = 0;
            parse_redirect(&stages[n]);
            n++;
        }

        if (!bar) {
            break;
        }
        start = bar + 1;
    }

    return n;
}

static void run_pipeline(char *line) {
    shell_stage_t stages[MAX_STAGES];
    char pipe_buf[CAPTURE_MAX];
    size_t pipe_len = 0;

    int n = parse_pipeline(line, stages, MAX_STAGES);
    if (n == 0) {
        return;
    }

    for (int i = 0; i < n; i++) {
        int has_redirect = stages[i].redirect[0] != 0;
        int has_pipe = (i + 1 < n);
        int capture = has_redirect || has_pipe;

        if (pipe_len > 0) {
            fos_api_set_pipe(pipe_buf, pipe_len);
        } else {
            fos_api_clear_pipe();
        }

        if (capture) {
            char cap_buf[CAPTURE_MAX];
            console_begin_capture(cap_buf, sizeof(cap_buf));
            run_builtin(stages[i].cmd);
            size_t out_len = console_end_capture();

            if (has_redirect) {
                if (vfs_write_file(vfs_get_drive(), stages[i].redirect, cap_buf, out_len) != 0) {
                    console_write_line("Redirect failed");
                }
                pipe_len = 0;
            } else {
                pipe_len = out_len;
                if (pipe_len >= sizeof(pipe_buf)) {
                    pipe_len = sizeof(pipe_buf) - 1;
                }
                memcpy(pipe_buf, cap_buf, pipe_len);
                pipe_buf[pipe_len] = 0;
            }
        } else {
            run_builtin(stages[i].cmd);
        }
    }

    fos_api_clear_pipe();
}

static void run_command(char *line) {
    line = (char *)skip_spaces(line);
    if (*line == 0) {
        return;
    }
    run_pipeline(line);
}

static void line_reset(void) {
    line_len = 0;
    line_buf[0] = 0;
}

static void line_backspace(void) {
    if (line_len > 0) {
        line_len--;
        line_buf[line_len] = 0;
        console_backspace();
    }
}

static void line_append(char c) {
    if (line_len + 1 >= LINE_MAX) {
        return;
    }
    line_buf[line_len++] = c;
    line_buf[line_len] = 0;
    console_putchar(c);
}

static void line_replace_from(int start, const char *replacement) {
    while (line_len > start) {
        line_backspace();
    }
    if (!replacement) {
        replacement = "";
    }
    for (const char *p = replacement; *p; p++) {
        line_append(*p);
    }
}

static void line_set(const char *text) {
    while (line_len > 0) {
        line_backspace();
    }
    if (!text) {
        text = "";
    }
    for (const char *p = text; *p; p++) {
        line_append(*p);
    }
}

static void hist_add(const char *line) {
    if (!line || !line[0]) {
        return;
    }
    if (hist_len > 0 && strcmp(line, hist_buf[hist_len - 1]) == 0) {
        return;
    }
    if (hist_len < HIST_MAX) {
        strcpy(hist_buf[hist_len++], line);
    } else {
        for (int i = 1; i < HIST_MAX; i++) {
            strcpy(hist_buf[i - 1], hist_buf[i]);
        }
        strcpy(hist_buf[HIST_MAX - 1], line);
    }
}

static void hist_up(void) {
    if (hist_len == 0) {
        return;
    }
    if (hist_pos < 0) {
        strcpy(hist_draft, line_buf);
        hist_pos = 0;
    } else if (hist_pos < hist_len - 1) {
        hist_pos++;
    } else {
        return;
    }
    line_set(hist_buf[hist_len - 1 - hist_pos]);
}

static void hist_down(void) {
    if (hist_pos < 0) {
        return;
    }
    if (hist_pos > 0) {
        hist_pos--;
        line_set(hist_buf[hist_len - 1 - hist_pos]);
    } else {
        hist_pos = -1;
        line_set(hist_draft);
    }
}

static int word_start_index(void) {
    int i = line_len - 1;
    while (i >= 0 && line_buf[i] != ' ' && line_buf[i] != '\t' &&
           line_buf[i] != '|' && line_buf[i] != '>') {
        i--;
    }
    return i + 1;
}

static size_t str_common_prefix(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && b[i]) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            break;
        }
        i++;
    }
    return i;
}

static size_t matches_common_prefix(vfs_complete_result_t *res) {
    if (res->count == 0) {
        return 0;
    }
    size_t n = strlen(res->names[0]);
    for (int i = 1; i < res->count; i++) {
        size_t c = str_common_prefix(res->names[0], res->names[i]);
        if (c < n) {
            n = c;
        }
    }
    return n;
}

static void line_complete(void) {
    int ws = word_start_index();
    char partial[LINE_MAX];
    strncpy(partial, line_buf + ws, sizeof(partial) - 1);
    partial[sizeof(partial) - 1] = 0;

    vfs_complete_result_t res;
    if (vfs_complete(vfs_get_drive(), partial, &res) != 0 || res.count == 0) {
        return;
    }

    size_t plen = strlen(partial);
    const char *last_slash = partial;
    for (const char *p = partial; *p; p++) {
        if (*p == '\\') {
            last_slash = p + 1;
        }
    }
    size_t name_off = (size_t)(last_slash - partial);
    size_t name_plen = plen - name_off;

    if (res.count == 1) {
        char full[LINE_MAX];
        size_t dir_len = name_off;
        if (dir_len > 0) {
            memcpy(full, partial, dir_len);
            full[dir_len] = 0;
            strncat(full, res.names[0], sizeof(full) - dir_len - 1);
        } else {
            strncpy(full, res.names[0], sizeof(full) - 1);
            full[sizeof(full) - 1] = 0;
        }
        line_replace_from(ws, full);
        return;
    }

    size_t common = matches_common_prefix(&res);
    if (common > name_plen) {
        char ext[64];
        strncpy(ext, res.names[0], common);
        ext[common] = 0;
        char full[LINE_MAX];
        size_t dir_len = name_off;
        if (dir_len > 0) {
            memcpy(full, partial, dir_len);
            full[dir_len] = 0;
            strncat(full, ext, sizeof(full) - dir_len - 1);
        } else {
            strncpy(full, ext, sizeof(full) - 1);
            full[sizeof(full) - 1] = 0;
        }
        line_replace_from(ws, full);
        return;
    }

    console_putchar('\n');
    for (int i = 0; i < res.count; i++) {
        console_write("  ");
        console_write_line(res.names[i]);
    }
    print_prompt();
    console_write(line_buf);
}

void shell_run(void) {
    print_banner();
    print_prompt();

    for (;;) {
        if (!keyboard_has_key()) {
            __asm__ volatile("pause");
            continue;
        }

        key_event_t ev = keyboard_read_event();
        if (ev.type == KEY_NONE) {
            continue;
        }

        if (ev.type == KEY_ENTER) {
            console_putchar('\n');
            hist_add(line_buf);
            hist_pos = -1;
            run_command(line_buf);
            line_reset();
            print_prompt();
            continue;
        }

        if (ev.type == KEY_BACKSPACE) {
            line_backspace();
            hist_pos = -1;
            continue;
        }

        if (ev.type == KEY_TAB) {
            line_complete();
            continue;
        }

        if (ev.type == KEY_UP) {
            hist_up();
            continue;
        }

        if (ev.type == KEY_DOWN) {
            hist_down();
            continue;
        }

        if (ev.type == KEY_CHAR && ev.ch >= 32 && ev.ch <= 126) {
            hist_pos = -1;
            line_append(ev.ch);
        }
    }
}
