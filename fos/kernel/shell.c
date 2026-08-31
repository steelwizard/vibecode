/*
 * shell.c — DOS-style shell with .COM programs, pipes (|), and redirect (>).
 *
 * Features:
 *   Built-in commands (help, dir, cd, type/cat, drives, ...)
 *   $NAME / ${NAME} expansion, NAME=value, i++ / i=i+1, env / set / unset
 *   External .COM programs via FOSCOM loader (echo.com, etc.)
 *   Pipelines: capture stdout of stage N, feed as pipe_in to stage N+1
 *   Redirect: capture stdout and write to a file on FAT32
 *   Command history (Up/Down) and Tab filename completion
 *   if / for / while, and .BAT files (line by line)
 */

#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "vfs.h"
#include "block.h"
#include "boot_report.h"
#include "foscom.h"
#include "fos_api.h"
#include "env.h"
#include "heap.h"
#include "mouse.h"
#include "string.h"

#define LINE_MAX       256
#define CAPTURE_MAX    32768
#define MAX_STAGES     8
#define HIST_MAX       32
#define SCRIPT_MAX     64
#define BAT_FILE_MAX   8192
#define BAT_NEST       4
#define SHELL_NEST     4
#define LOOP_MAX       10000
#define FLOW_NEXT      0
#define FLOW_BREAK     1
#define FLOW_CONT      2

typedef struct {
    char cmd[LINE_MAX];
    char redirect[VFS_PATH_MAX];
} shell_stage_t;

static char line_buf[LINE_MAX];
static int line_len = 0;
static int line_cursor = 0; /* insertion point within line_buf (0..line_len) */

/* In-memory command history ring buffer (not persisted). */
static char hist_buf[HIST_MAX][LINE_MAX];
static int hist_len = 0;
static int hist_pos = -1;       /* -1 = editing live line, 0 = newest recalled */
static char hist_draft[LINE_MAX]; /* saved partial line before Up arrow */

static int last_status;
static char script_lines[SCRIPT_MAX][LINE_MAX];
static int script_n;
static int script_depth;
static int bat_depth;
static int prompt_cols;
static int shell_depth;
static int shell_leave;

static void set_status(int st);
static void script_reset(void);
static int try_run_bat(const char *line);
static int exec_range(int lo, int hi);

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
    console_write_line("Use | to pipe, > to redirect, and if / for / while for scripts.");
    console_write_line("");
}

static void print_prompt(void) {
    char prompt[64];
    if (script_depth > 0) {
        console_set_color(11, 0);
        console_write("> ");
        console_set_color(15, 0);
        return;
    }
    vfs_format_prompt(prompt, sizeof(prompt));
    prompt_cols = (int)strlen(prompt);
    /* Light green on success, light red after a failed command. */
    console_set_color(last_status ? 12 : 10, 0);
    console_write(prompt);
    console_set_color(15, 0);
}

static void shell_error2(const char *a, const char *b) {
    char buf[192];
    size_t n = 0;

    while (a && *a && n + 1 < sizeof(buf)) {
        buf[n++] = *a++;
    }
    while (b && *b && n + 1 < sizeof(buf)) {
        buf[n++] = *b++;
    }
    buf[n] = 0;
    set_status(1);
    console_error(buf);
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

static void int_to_dec(int v, char *buf, size_t cap) {
    char tmp[16];
    int n = 0;
    int neg = 0;
    unsigned u;
    int i;

    if (!buf || cap == 0) {
        return;
    }
    if (v < 0) {
        neg = 1;
        u = (unsigned)(-v);
    } else {
        u = (unsigned)v;
    }
    if (u == 0) {
        tmp[n++] = '0';
    }
    while (u && n < (int)sizeof(tmp) - 1) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg) {
        tmp[n++] = '-';
    }
    if ((size_t)n + 1 > cap) {
        n = (int)cap - 1;
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = 0;
}

static void set_status(int st) {
    char buf[12];

    if (st < 0) {
        st = 1;
    }
    last_status = st;
    int_to_dec(st, buf, sizeof(buf));
    env_set("ERRORLEVEL", buf);
}

static int line_kw(const char *line, const char *kw) {
    size_t n;
    line = skip_spaces(line);
    n = strlen(kw);
    if (strncasecmp(line, kw, n) != 0) {
        return 0;
    }
    return line[n] == 0 || line[n] == ' ' || line[n] == '\t';
}

static const char *after_kw(const char *line, const char *kw) {
    line = skip_spaces(line);
    return skip_spaces(line + strlen(kw));
}

static const char *take_token(const char *s, char *out, size_t cap) {
    size_t n = 0;

    s = skip_spaces(s);
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && n + 1 < cap) {
            out[n++] = *s++;
        }
        if (*s == '"') {
            s++;
        }
        out[n] = 0;
        return s;
    }
    while (*s && *s != ' ' && *s != '\t' && *s != '=' && *s != '(' && *s != ')' &&
           *s != '<' && *s != '>' && *s != '!' && n + 1 < cap) {
        out[n++] = *s++;
    }
    out[n] = 0;
    return s;
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

/* Uppercase name and append ext (".COM", ".BAT") when no extension is present. */
static void make_prog_name(const char *name, const char *ext, char *base, size_t base_sz) {
    size_t i = 0;
    const char *n = name;

    while (*n == ' ' || *n == '\t') {
        n++;
    }
    while (*n && i + 1 < base_sz) {
        char c = *n++;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        base[i++] = c;
    }
    base[i] = 0;

    if (!strchr(base, '.')) {
        strncat(base, ext, base_sz - strlen(base) - 1);
    }
}

static int name_has_ext(const char *name, const char *ext) {
    char base[64];
    size_t n;
    size_t e;

    make_prog_name(name, ext, base, sizeof(base));
    n = strlen(base);
    e = strlen(ext);
    if (n < e) {
        return 0;
    }
    return strcmp(base + n - e, ext) == 0;
}

static int path_join(char *out, size_t out_sz, const char *dir, const char *file) {
    size_t n = 0;
    const char *d = dir;

    if (!out || out_sz == 0 || !file || !file[0]) {
        return -1;
    }

    while (*d == ' ' || *d == '\t') {
        d++;
    }
    if (*d == 0) {
        d = "\\";
    }

    if (*d != '\\') {
        if (n + 1 >= out_sz) {
            return -1;
        }
        out[n++] = '\\';
    }
    while (*d && n + 1 < out_sz) {
        out[n++] = *d++;
    }
    /* Trim trailing slashes, but keep a single root "\". */
    while (n > 1 && out[n - 1] == '\\') {
        n--;
    }
    if (!(n == 1 && out[0] == '\\')) {
        if (n + 1 >= out_sz) {
            return -1;
        }
        out[n++] = '\\';
    }
    while (*file && n + 1 < out_sz) {
        out[n++] = *file++;
    }
    out[n] = 0;
    return 0;
}

static int com_exists(const char *path) {
    return vfs_locate_file(vfs_get_drive(), path) >= 0;
}

/*
 * Resolve a .COM program name using $PATH (colon-separated dirs).
 * Seeded from [shell] path= in SYSTEM.INI. Absolute names (leading \) are
 * used as-is. Otherwise: cwd, then each PATH entry.
 */
static int resolve_prog(const char *name, const char *ext, char *out, size_t out_sz) {
    char base[64];
    char pathenv[256];
    const char *cfg;
    const char *p;

    if (!name || !name[0] || !out || out_sz == 0) {
        return -1;
    }

    make_prog_name(name, ext, base, sizeof(base));
    if (base[0] == 0) {
        return -1;
    }
    if (strchr(name, '.') && !name_has_ext(name, ext)) {
        return -1;
    }

    /* Absolute or nested path: use from drive root. */
    if (name[0] == '\\' || strchr(base, '\\')) {
        if (base[0] == '\\') {
            strncpy(out, base, out_sz - 1);
        } else {
            out[0] = '\\';
            strncpy(out + 1, base, out_sz - 2);
        }
        out[out_sz - 1] = 0;
        return com_exists(out) ? 0 : -1;
    }

    /* Current working directory. */
    strncpy(out, base, out_sz - 1);
    out[out_sz - 1] = 0;
    if (com_exists(out)) {
        return 0;
    }

    cfg = env_get("PATH");
    if (cfg && cfg[0]) {
        strncpy(pathenv, cfg, sizeof(pathenv) - 1);
        pathenv[sizeof(pathenv) - 1] = 0;
    } else {
        strcpy(pathenv, "\\FOS");
    }

    p = pathenv;
    while (*p) {
        char dir[VFS_PATH_MAX];
        size_t di = 0;

        while (*p == ':') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        while (*p && *p != ':' && di + 1 < sizeof(dir)) {
            dir[di++] = *p++;
        }
        dir[di] = 0;
        while (di > 0 && (dir[di - 1] == ' ' || dir[di - 1] == '\t')) {
            dir[--di] = 0;
        }

        if (dir[0] == 0) {
            continue;
        }
        if (path_join(out, out_sz, dir, base) != 0) {
            continue;
        }
        if (com_exists(out)) {
            return 0;
        }
    }

    return -1;
}

static int resolve_com(const char *name, char *out, size_t out_sz) {
    return resolve_prog(name, ".COM", out, out_sz);
}

static void cmd_help(void) {
    console_write_line("Commands (case insensitive):");
    console_write_line("  help                 Command list");
    console_write_line("  ver                  Version");
    console_write_line("  cls / clear          Clear screen");
    console_write_line("  dir [path]           List directory");
    console_write_line("  cd <path>            Change directory");
    console_write_line("  pwd / which <name>   Directory; locate .COM / .BAT / builtin");
    console_write_line("  mkdir / md <path>    Create directory");
    console_write_line("  del / erase <path>   Delete file or empty folder");
    console_write_line("  copy <src> <dst>     Copy file");
    console_write_line("  move / ren <src> <dst>  Move or rename file");
    console_write_line("  type / cat <path>    Print file (or piped text)");
    console_write_line("  drives / df          List drives");
    console_write_line("  <n>:                 Switch drive");
    console_write_line("  reboot               Reboot");
    console_write_line("  <name>.com [args]    Run .COM program (e.g. echo hello)");
    console_write_line("  <name>.bat [args]    Run a .BAT script (also: call name)");
    console_write_line("  edit [file]          Text editor (Ctrl+S save, Ctrl+X exit)");
    console_write_line("  less [file]          Page through a file (q to quit)");
    console_write_line("  fm                   File manager (m = mkdir)");
    console_write_line("  date [when]          Show or set RTC (date.com)");
    console_write_line("  mem                  RAM map and usage (mem.com)");
    console_write_line("  beep [hz [ms]|file]  Sound Blaster beep (beep.com)");
    console_write_line("  play <file>          WAV/MP3/MIDI (play MIDI\\PREL1.MID)");
    console_write_line("  paint [file]         Mouse paint (s save, o open, q quit)");
    console_write_line("  grep [-inv] PAT [file]  Find lines (fixed string)");
    console_write_line("  bench / test          Test bench (primes, soak, mem, gfx, audio, hw)");
    console_write_line("  tetris                Tetromino game (arrows, z/x, space, c hold)");
    console_write_line("  shell / exit          Nested shell; exit returns");
    console_write_line("  cmd1 | cmd2          Pipe stdout to stdin");
    console_write_line("  cmd > file           Redirect stdout to file");
    console_write_line("  Up/Down/Left/Right  Edit command line");
    console_write_line("  PgUp/PgDn            Scroll terminal output");
    console_write_line("  | and >              Pipe / redirect (DE: AltGr+< is |, Shift+< is >)");
    console_write_line("  Tab                  Filename completion");
    console_write_line("  Ctrl+C               Cancel line / stop dir&type/cat");
    console_write_line("  NAME=value            Set $NAME (i++ i=i+1 i+=n)");
    console_write_line("  env / set             List environment");
    console_write_line("  unset NAME            Remove variable");
    console_write_line("  echo $PATH            Expand $NAME or ${NAME} (not in 'quotes')");
    console_write_line("  echo $(1+5)           Integer math (+ - * / %; names ok)");
    console_write_line("  PATH                  $PATH for .COM / .BAT lookup (default \\FOS)");
    console_write_line("  echo %1 / %PATH%      .BAT args %0..%9 %* and %NAME%");
    console_write_line("  if exist f / i < y    Also errorlevel, true/false, == != <= >=");
    console_write_line("  for i in a b do cmd   Or: for i = 1 to 10 do cmd");
    console_write_line("  while true do cmd     Blocks: omit then/do, indent, close with end");
    console_write_line("  break / continue      Leave or restart the innermost for/while");
    console_write_line("  true / false          Set ERRORLEVEL to 0 or 1");
}

static void cmd_ver(void) {
    console_write_line("FOS v0.3 - Flash Operating System");
    console_write_line("FOSCOM .COM programs, .BAT scripts, pipes, redirect");
}

static void cmd_drives(void) {
    boot_print_ata_disks();
    console_write_line("");
    vfs_print_drive_table();
}

static void cmd_dir(const char *args) {
    const char *path = args[0] ? args : vfs_get_cwd();
    if (vfs_list_dir(vfs_get_drive(), path) != 0) {
        set_status(1);
        console_error("DIR failed");
    }
}

static void cmd_cd(const char *args) {
    if (*args == 0) {
        const char *home = env_get("HOME");
        args = (home && home[0]) ? home : "\\";
    }
    if (vfs_set_cwd(args) != 0) {
        set_status(1);
        console_error("CD failed");
        return;
    }
    env_sync_pwd();
}

static void write_drive_path(const char *rel) {
    char shown[VFS_PATH_MAX + 8];
    char joined[VFS_PATH_MAX];
    const char *path = rel;
    size_t n = 0;
    int d = vfs_get_drive();

    if (rel && rel[0] && rel[1] == ':') {
        console_write_line(rel);
        return;
    }
    if (!rel) {
        rel = "\\";
    }
    if (rel[0] != '\\') {
        if (path_join(joined, sizeof(joined), vfs_get_cwd(), rel) == 0) {
            path = joined;
        }
    }
    shown[n++] = (char)('0' + d);
    shown[n++] = ':';
    while (*path && n + 1 < sizeof(shown)) {
        shown[n++] = *path++;
    }
    shown[n] = 0;
    console_write_line(shown);
}

static void cmd_pwd(void) {
    const char *pwd;

    env_sync_pwd();
    pwd = env_get("PWD");
    if (pwd && pwd[0]) {
        console_write_line(pwd);
        return;
    }
    write_drive_path(vfs_get_cwd());
}

static int is_shell_builtin(const char *name) {
    static const char *const names[] = {
        "help", "?", "ver", "cls", "clear", "dir", "cd", "pwd", "which", "where",
        "mkdir", "md", "del", "erase", "copy", "move", "ren", "rename",
        "type", "cat", "drives", "df", "reboot", "call",
        "true", "false", "env", "set", "unset", "export",
        "shell", "exit",
        0
    };
    int i;

    for (i = 0; names[i]; i++) {
        if (strcasecmp(name, names[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static const char *which_alias_com(const char *name) {
    if (strcasecmp(name, "edit") == 0) {
        return "EDIT";
    }
    if (strcasecmp(name, "fm") == 0) {
        return "FM";
    }
    if (strcasecmp(name, "less") == 0 || strcasecmp(name, "more") == 0) {
        return "LESS";
    }
    if (strcasecmp(name, "play") == 0) {
        return "PLAY";
    }
    if (strcasecmp(name, "paint") == 0) {
        return "PAINT";
    }
    if (strcasecmp(name, "bench") == 0 || strcasecmp(name, "test") == 0) {
        return "BENCH";
    }
    if (strcasecmp(name, "tetris") == 0) {
        return "TETRIS";
    }
    return 0;
}

static int which_one(const char *name) {
    char path[VFS_PATH_MAX];
    const char *com;

    if (!name || !name[0]) {
        return -1;
    }
    com = which_alias_com(name);
    if (com && resolve_com(com, path, sizeof(path)) == 0) {
        write_drive_path(path);
        return 0;
    }
    if (is_shell_builtin(name)) {
        console_write(name);
        console_write_line(": shell builtin");
        return 0;
    }
    if (resolve_com(name, path, sizeof(path)) == 0) {
        write_drive_path(path);
        return 0;
    }
    if (resolve_prog(name, ".BAT", path, sizeof(path)) == 0) {
        write_drive_path(path);
        return 0;
    }
    console_write("which: ");
    console_write(name);
    console_write_line(": not found");
    return -1;
}

static void cmd_which(const char *args) {
    char tok[64];
    const char *s = args;
    int any = 0;
    int miss = 0;

    for (;;) {
        s = take_token(s, tok, sizeof(tok));
        if (tok[0] == 0) {
            break;
        }
        any = 1;
        if (which_one(tok) != 0) {
            miss = 1;
        }
    }
    if (!any) {
        set_status(1);
        console_error("WHICH: name required");
        return;
    }
    if (miss) {
        set_status(1);
    }
}

static int confirm_yes_no(void) {
    console_write(" (Y/N)? ");
    for (;;) {
        while (!keyboard_has_key()) {
            __asm__ volatile("pause");
        }
        key_event_t ev = keyboard_read_event();
        if (ev.type == KEY_CHAR) {
            if (ev.ch == 3) {
                console_write_line("^C");
                return 0;
            }
            if (ev.ch == 'y' || ev.ch == 'Y') {
                console_write_line("Y");
                return 1;
            }
            if (ev.ch == 'n' || ev.ch == 'N') {
                console_write_line("N");
                return 0;
            }
        }
        if (ev.type == KEY_ENTER) {
            console_putchar('\n');
            return 0;
        }
    }
}

static void cmd_mkdir(const char *args) {
    if (*args == 0) {
        set_status(1);
        console_error("MKDIR: path required");
        return;
    }
    if (vfs_mkdir(vfs_get_drive(), args) != 0) {
        set_status(1);
        console_error("MKDIR failed");
    }
}

static void cmd_del(const char *args) {
    if (*args == 0) {
        set_status(1);
        console_error("DEL: path required");
        return;
    }
    console_write("Delete ");
    console_write(args);
    if (!confirm_yes_no()) {
        console_write_line("Cancelled");
        return;
    }
    int rc = vfs_delete(vfs_get_drive(), args);
    if (rc == -2) {
        set_status(1);
        console_error("Directory not empty");
    } else if (rc != 0) {
        set_status(1);
        console_error("DEL failed");
    }
}

static int split_two_paths(const char *args, char *first, size_t first_sz,
                           char *second, size_t second_sz) {
    args = skip_spaces(args);
    size_t i = 0;
    while (*args && *args != ' ' && i + 1 < first_sz) {
        first[i++] = *args++;
    }
    first[i] = 0;
    args = skip_spaces(args);
    strncpy(second, args, second_sz - 1);
    second[second_sz - 1] = 0;
    trim(second);
    return first[0] != 0 && second[0] != 0;
}

static void cmd_copy(const char *args) {
    char src[VFS_PATH_MAX];
    char dst[VFS_PATH_MAX];

    if (!split_two_paths(args, src, sizeof(src), dst, sizeof(dst))) {
        set_status(1);
        console_error("COPY: source and destination required");
        return;
    }
    int rc = vfs_copy(vfs_get_drive(), src, dst);
    if (rc == -2) {
        set_status(1);
        console_error("COPY failed: out of memory");
    } else if (rc != 0) {
        set_status(1);
        console_error("COPY failed");
    } else {
        console_write_line("        1 file(s) copied");
    }
}

static void cmd_move(const char *args) {
    char src[VFS_PATH_MAX];
    char dst[VFS_PATH_MAX];

    if (!split_two_paths(args, src, sizeof(src), dst, sizeof(dst))) {
        set_status(1);
        console_error("MOVE: source and destination required");
        return;
    }
    int rc = vfs_move(vfs_get_drive(), src, dst);
    if (rc == -2) {
        set_status(1);
        console_error("MOVE failed: out of memory");
    } else if (rc == -3) {
        set_status(1);
        console_error("Copied, but could not remove the original");
    } else if (rc != 0) {
        set_status(1);
        console_error("MOVE failed");
    } else {
        console_write_line("        1 file(s) moved");
    }
}

static void cmd_type(const char *args) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    char chunk[512];
    char last = 0;
    int any = 0;

    if (*args == 0 && api->pipe_in_len > 0) {
        console_write_n(api->pipe_in, api->pipe_in_len);
        if (api->pipe_in[api->pipe_in_len - 1] != '\n') {
            console_putchar('\n');
        }
        return;
    }

    if (*args == 0) {
        set_status(1);
        console_error("TYPE/CAT: file required");
        return;
    }

    {
        vfs_file_t f;
        uint32_t got;

        if (vfs_open(vfs_get_drive(), args, VFS_O_READ, &f) != 0) {
            set_status(1);
            console_error("TYPE/CAT failed");
            return;
        }
        for (;;) {
            got = 0;
            if (keyboard_check_ctrl_c()) {
                console_write_line("^C");
                set_status(1);
                vfs_close(&f);
                return;
            }
            if (vfs_read(&f, chunk, (uint32_t)sizeof(chunk), &got) != 0) {
                if (!any) {
                    set_status(1);
                    console_error("TYPE/CAT failed");
                }
                vfs_close(&f);
                return;
            }
            if (got == 0) {
                break;
            }
            console_write_n(chunk, got);
            last = chunk[got - 1];
            any = 1;
            if (got < sizeof(chunk)) {
                break;
            }
        }
        vfs_close(&f);
    }
    if (any && last != '\n') {
        console_putchar('\n');
    }
}

static int try_drive_switch(char *line) {
    const char *s = skip_spaces(line);
    if (s[0] >= '0' && s[0] <= '9' && s[1] == ':') {
        const char *rest = skip_spaces(s + 2);
        if (*rest != 0) {
            return 0;
        }
        int d = s[0] - '0';
        if (vfs_set_drive(d) == 0) {
            env_sync_pwd();
            return 1;
        }
    }
    return 0;
}

static void cmd_exit(void) {
    if (shell_depth <= 1) {
        console_write_line("Cannot exit the root shell");
        set_status(1);
        return;
    }
    shell_leave = 1;
}

static void cmd_shell(void) {
    shell_run();
}

static int name_is_shell_com(const char *name) {
    const char *base = name;
    const char *p;

    if (!name || !name[0]) {
        return 0;
    }
    for (p = name; *p; p++) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    return strcasecmp(base, "shell") == 0 ||
           strcasecmp(base, "shell.com") == 0 ||
           strcasecmp(base, "command") == 0 ||
           strcasecmp(base, "command.com") == 0;
}

static int try_run_com(const char *line) {
    char name[64];
    char args[256];
    char path[VFS_PATH_MAX];

    split_cmd(line, name, sizeof(name), args, sizeof(args));
    if (name[0] == 0) {
        return 0;
    }
    if (name_is_shell_com(name)) {
        cmd_shell();
        return 1;
    }
    if (resolve_com(name, path, sizeof(path)) != 0) {
        return 0;
    }
    if (name_is_shell_com(path)) {
        cmd_shell();
        return 1;
    }
    return exec_run(vfs_get_drive(), path, args) == 0;
}

static int bat_echo_toggle(const char *line) {
    const char *a;

    if (bat_depth <= 0 || !line_kw(line, "echo")) {
        return 0;
    }
    a = skip_spaces(cmd_arg(line));
    return strcasecmp(a, "on") == 0 || strcasecmp(a, "off") == 0;
}

static int bat_parse_lines(const char *text, size_t len, char dest[][LINE_MAX],
                           int cap, int *out_n) {
    size_t i = 0;
    int n = 0;

    if (len >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        i = 3;
    }

    while (i < len) {
        char line[LINE_MAX];
        size_t L = 0;

        while (i < len && text[i] != '\n' && text[i] != '\r') {
            if (L + 1 < sizeof(line)) {
                line[L++] = text[i];
            }
            i++;
        }
        if (i < len && text[i] == '\r') {
            i++;
        }
        if (i < len && text[i] == '\n') {
            i++;
        }
        line[L] = 0;
        trim(line);
        if (line[0] == '@') {
            memmove(line, line + 1, strlen(line + 1) + 1);
            trim(line);
        }
        if (line[0] == 0) {
            continue;
        }
        if (n >= cap) {
            return -1;
        }
        strncpy(dest[n], line, LINE_MAX - 1);
        dest[n][LINE_MAX - 1] = 0;
        n++;
    }
    *out_n = n;
    return 0;
}

static int run_bat_file(const char *path, const char *args) {
    uint32_t size = 0;
    int is_dir = 0;
    char *text;
    size_t nread = 0;
    char (*saved_lines)[LINE_MAX];
    int saved_n;
    int saved_depth;
    env_params_t saved_params;
    int nlines = 0;

    if (bat_depth >= BAT_NEST) {
        console_error("Too many nested .BAT files");
        set_status(1);
        return 1;
    }
    if (vfs_stat(vfs_get_drive(), path, &size, &is_dir) != 0 || is_dir) {
        return 0;
    }
    if (size > BAT_FILE_MAX) {
        console_error(".BAT file too large");
        set_status(1);
        return 1;
    }

    saved_lines = heap_alloc(sizeof(script_lines));
    if (!saved_lines) {
        console_error("Out of memory");
        set_status(1);
        return 1;
    }
    memcpy(saved_lines, script_lines, sizeof(script_lines));
    saved_n = script_n;
    saved_depth = script_depth;
    env_params_save(&saved_params);

    text = 0;
    if (size > 0) {
        text = heap_alloc((size_t)size + 1);
        if (!text) {
            heap_free(saved_lines);
            console_error("Out of memory");
            set_status(1);
            return 1;
        }
        {
            vfs_file_t f;
            uint32_t got = 0;

            if (vfs_open(vfs_get_drive(), path, VFS_O_READ, &f) != 0 ||
                vfs_read(&f, text, size, &got) != 0) {
                vfs_close(&f);
                heap_free(text);
                heap_free(saved_lines);
                console_error("Cannot read .BAT");
                set_status(1);
                return 1;
            }
            vfs_close(&f);
            nread = got;
        }
        text[nread] = 0;
    }

    if (bat_parse_lines(text ? text : "", nread, script_lines, SCRIPT_MAX, &nlines) != 0) {
        if (text) {
            heap_free(text);
        }
        memcpy(script_lines, saved_lines, sizeof(script_lines));
        heap_free(saved_lines);
        console_error("Script too long (64 lines)");
        set_status(1);
        return 1;
    }
    if (text) {
        heap_free(text);
    }

    script_n = nlines;
    script_depth = 0;
    env_set_params(path, args);
    bat_depth++;
    exec_range(0, script_n);
    bat_depth--;

    memcpy(script_lines, saved_lines, sizeof(script_lines));
    script_n = saved_n;
    script_depth = saved_depth;
    env_params_load(&saved_params);
    heap_free(saved_lines);
    return 1;
}

int shell_run_bat(const char *path, const char *args) {
    if (!path || !path[0]) {
        return -1;
    }
    return run_bat_file(path, args ? args : "") ? 0 : -1;
}

static int try_run_bat(const char *line) {
    char name[64];
    char args[256];
    char path[VFS_PATH_MAX];

    split_cmd(line, name, sizeof(name), args, sizeof(args));
    if (name[0] == 0) {
        return 0;
    }
    if (resolve_prog(name, ".BAT", path, sizeof(path)) != 0) {
        return 0;
    }
    return run_bat_file(path, args);
}

static int run_direct_com(const char *prog, const char *args, const char *fail_msg) {
    char path[VFS_PATH_MAX];

    if (resolve_com(prog, path, sizeof(path)) != 0) {
        console_error(fail_msg);
        set_status(1);
        return -1;
    }
    console_begin_direct();
    if (exec_run(vfs_get_drive(), path, args) != 0) {
        console_end_direct();
        console_error(fail_msg);
        set_status(1);
        return -1;
    }
    console_end_direct();
    return 0;
}

static void cmd_edit(const char *args) {
    run_direct_com("EDIT", args,
                   "EDIT failed (missing or corrupt EDIT.COM — run: make clean && make)");
}

static void cmd_fm(const char *args) {
    run_direct_com("FM", args,
                   "FM failed (missing or corrupt FM.COM — run: make clean && make)");
}

static void cmd_less(const char *args) {
    run_direct_com("LESS", args,
                   "LESS failed (missing or corrupt LESS.COM — run: make clean && make)");
}

static void cmd_play(const char *args) {
    run_direct_com("PLAY", args,
                   "PLAY failed (missing or corrupt PLAY.COM — run: make clean && make)");
}

static void cmd_paint(const char *args) {
    run_direct_com("PAINT", args,
                   "PAINT failed (missing or corrupt PAINT.COM — run: make clean && make)");
}

static void cmd_bench(const char *args) {
    run_direct_com("BENCH", args,
                   "BENCH failed (missing or corrupt BENCH.COM — run: make clean && make)");
}

static void cmd_tetris(const char *args) {
    run_direct_com("TETRIS", args,
                   "TETRIS failed (missing or corrupt TETRIS.COM — run: make clean && make)");
}

static int copy_ident(const char *p, char *name, size_t name_sz, const char **rest) {
    size_t n = 0;

    if (!env_name_start((unsigned char)*p)) {
        return 0;
    }
    while (env_name_char((unsigned char)*p) && n + 1 < name_sz) {
        name[n++] = *p++;
    }
    if (env_name_char((unsigned char)*p)) {
        return 0;
    }
    name[n] = 0;
    *rest = p;
    return n > 0;
}

/* NAME=value, export NAME[=value], unset NAME, env, set.
 * Also: i++ / ++i / i-- / --i, i+=n (and -= *= /= %=), and i=i+1. */
static int try_env_command(char *line) {
    const char *p = skip_spaces(line);
    char name[ENV_NAME_MAX];
    const char *rest;
    int exporting = 0;
    int64_t n;
    int64_t cur;
    char op;

    if (match_cmd(p, "ENV") || (match_cmd(p, "SET") && cmd_arg(p)[0] == 0)) {
        env_print();
        return 1;
    }

    if (match_cmd(p, "UNSET")) {
        p = skip_spaces(cmd_arg(p));
        if (!copy_ident(p, name, sizeof(name), &rest) || *skip_spaces(rest)) {
            console_error("unset NAME");
            set_status(1);
            return 1;
        }
        if (env_unset(name) != 0) {
            shell_error2("unset: no such variable: ", name);
        }
        return 1;
    }

    if (match_cmd(p, "EXPORT")) {
        exporting = 1;
        p = skip_spaces(cmd_arg(p));
        if (*p == 0) {
            env_print();
            return 1;
        }
    } else if (match_cmd(p, "SET")) {
        p = skip_spaces(cmd_arg(p));
        if (*p == 0) {
            env_print();
            return 1;
        }
    }

    /* ++i / --i */
    if ((p[0] == '+' || p[0] == '-') && p[1] == p[0] &&
        copy_ident(skip_spaces(p + 2), name, sizeof(name), &rest) &&
        *skip_spaces(rest) == 0) {
        if (env_get_i64(name, &cur) != 0 ||
            env_set_i64(name, cur + (p[0] == '+' ? 1 : -1)) != 0) {
            console_error("not a number");
            set_status(1);
        }
        return 1;
    }

    if (!copy_ident(p, name, sizeof(name), &rest)) {
        return 0;
    }
    rest = skip_spaces(rest);

    /* i++ / i-- */
    if ((rest[0] == '+' || rest[0] == '-') && rest[1] == rest[0] &&
        *skip_spaces(rest + 2) == 0) {
        if (env_get_i64(name, &cur) != 0 ||
            env_set_i64(name, cur + (rest[0] == '+' ? 1 : -1)) != 0) {
            console_error("not a number");
            set_status(1);
        }
        return 1;
    }

    /* i+=n i-=n i*=n i/=n i%=n */
    op = rest[0];
    if ((op == '+' || op == '-' || op == '*' || op == '/' || op == '%') &&
        rest[1] == '=') {
        rest = skip_spaces(rest + 2);
        if (env_arith_eval(rest, &n) != 0) {
            console_error("bad arithmetic");
            set_status(1);
            return 1;
        }
        if (env_get_i64(name, &cur) != 0) {
            console_error("not a number");
            set_status(1);
            return 1;
        }
        if ((op == '/' || op == '%') && n == 0) {
            console_error("division by zero");
            set_status(1);
            return 1;
        }
        if (op == '+') {
            cur += n;
        } else if (op == '-') {
            cur -= n;
        } else if (op == '*') {
            cur *= n;
        } else if (op == '/') {
            cur /= n;
        } else {
            cur %= n;
        }
        if (env_set_i64(name, cur) != 0) {
            console_error("Cannot set variable");
            set_status(1);
        }
        return 1;
    }

    if (*rest == '=') {
        rest++;
        if (env_try_arith(rest, &n) == 0) {
            if (env_set_i64(name, n) != 0) {
                console_error("Cannot set variable");
                set_status(1);
            }
        } else if (env_set(name, rest) != 0) {
            console_error("Cannot set variable");
            set_status(1);
        }
        return 1;
    }
    if (exporting || match_cmd(skip_spaces(line), "SET")) {
        const char *val = env_get(name);
        if (!val) {
            shell_error2("No such variable: ", name);
            return 1;
        }
        console_write(name);
        console_putchar('=');
        console_write_line(val);
        return 1;
    }
    return 0;
}

static int run_builtin(char *line) {
    set_status(0);
    if (try_drive_switch(line)) {
        return 1;
    }
    if (try_env_command(line)) {
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
    } else if (match_cmd(line, "PWD")) {
        cmd_pwd();
    } else if (match_cmd(line, "WHICH") || match_cmd(line, "WHERE")) {
        cmd_which(cmd_arg(line));
    } else if (match_cmd(line, "MKDIR") || match_cmd(line, "MD")) {
        cmd_mkdir(cmd_arg(line));
    } else if (match_cmd(line, "DEL") || match_cmd(line, "ERASE")) {
        cmd_del(cmd_arg(line));
    } else if (match_cmd(line, "COPY")) {
        cmd_copy(cmd_arg(line));
    } else if (match_cmd(line, "MOVE") || match_cmd(line, "REN") ||
               match_cmd(line, "RENAME")) {
        cmd_move(cmd_arg(line));
    } else if (match_cmd(line, "TYPE") || match_cmd(line, "CAT")) {
        cmd_type(cmd_arg(line));
    } else if (match_cmd(line, "DRIVES") || match_cmd(line, "DF")) {
        cmd_drives();
    } else if (match_cmd(line, "REBOOT")) {
        console_write_line("Rebooting...");
        reboot();
    } else if (match_cmd(line, "EDIT")) {
        cmd_edit(cmd_arg(line));
    } else if (match_cmd(line, "FM")) {
        cmd_fm(cmd_arg(line));
    } else if (match_cmd(line, "LESS") || match_cmd(line, "MORE")) {
        cmd_less(cmd_arg(line));
    } else if (match_cmd(line, "PLAY")) {
        cmd_play(cmd_arg(line));
    } else if (match_cmd(line, "PAINT")) {
        cmd_paint(cmd_arg(line));
    } else if (match_cmd(line, "BENCH") || match_cmd(line, "TEST")) {
        cmd_bench(cmd_arg(line));
    } else if (match_cmd(line, "TETRIS")) {
        cmd_tetris(cmd_arg(line));
    } else if (match_cmd(line, "SHELL") || match_cmd(line, "COMMAND")) {
        cmd_shell();
    } else if (match_cmd(line, "EXIT")) {
        cmd_exit();
    } else if (match_cmd(line, "TRUE")) {
        set_status(0);
    } else if (match_cmd(line, "FALSE")) {
        set_status(1);
    } else if (match_cmd(line, "CALL")) {
        const char *rest = cmd_arg(line);
        if (*rest == 0) {
            console_error("CALL: filename required");
            set_status(1);
        } else if (!try_run_bat(rest) && !try_run_com(rest)) {
            shell_error2("Unknown: ", rest);
        }
    } else if (try_run_com(line)) {
        return 1;
    } else if (try_run_bat(line)) {
        return 1;
    } else {
        shell_error2("Unknown: ", line);
    }
    return 1;
}

static char quote_push(char q, char c) {
    if (c != '"' && c != '\'') {
        return q;
    }
    if (!q) {
        return c;
    }
    if (q == c) {
        return 0;
    }
    return q;
}

static void strip_quotes(char *s) {
    char *d = s;
    if (!s) {
        return;
    }
    for (; *s; s++) {
        if (*s != '"' && *s != '\'') {
            *d++ = *s;
        }
    }
    *d = 0;
}

static void parse_redirect(shell_stage_t *st) {
    /* Split on '>' outside quotes. */
    char *gt = 0;
    char q = 0;
    for (char *p = st->cmd; *p; p++) {
        q = quote_push(q, *p);
        if (!q && *p == '>') {
            gt = p;
            break;
        }
    }
    st->redirect[0] = 0;
    if (!gt) {
        trim(st->cmd);
        strip_quotes(st->cmd);
        return;
    }

    *gt = 0;
    trim(st->cmd);
    strip_quotes(st->cmd);
    gt++;
    while (*gt == ' ' || *gt == '\t') {
        gt++;
    }
    strncpy(st->redirect, gt, sizeof(st->redirect) - 1);
    st->redirect[sizeof(st->redirect) - 1] = 0;
    trim(st->redirect);
    strip_quotes(st->redirect);
}

static int parse_pipeline(char *line, shell_stage_t *stages, int max) {
    int n = 0;
    char *start = line;

    while (n < max) {
        char *bar = 0;
        char q = 0;
        for (char *p = start; *p; p++) {
            q = quote_push(q, *p);
            if (!q && *p == '|') {
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
    static char pipe_buf[CAPTURE_MAX];
    size_t pipe_len = 0;

    int n = parse_pipeline(line, stages, MAX_STAGES);
    if (n == 0) {
        return;
    }

    for (int i = 0; i < n; i++) {
        int has_redirect = stages[i].redirect[0] != 0;
        int has_pipe = (i + 1 < n);
        int capture = has_redirect || has_pipe;

        if (keyboard_check_ctrl_c()) {
            console_write_line("^C");
            set_status(1);
            break;
        }

        /* Previous stage output becomes this stage's stdin (for .COM pipe_in). */
        if (pipe_len > 0) {
            fos_api_set_pipe(pipe_buf, pipe_len);
        } else {
            fos_api_clear_pipe();
        }

        if (capture) {
            static char cap_buf[CAPTURE_MAX];
            /* Intercept console_write* into cap_buf instead of the screen. */
            console_begin_capture(cap_buf, sizeof(cap_buf));
            run_builtin(stages[i].cmd);
            size_t out_len = console_end_capture();

            if (has_redirect) {
                vfs_file_t rf;
                uint32_t put = 0;
                int wflags = VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC;

                if (vfs_open(vfs_get_drive(), stages[i].redirect, wflags, &rf) != 0 ||
                    (out_len > 0 && (vfs_write(&rf, cap_buf, (uint32_t)out_len, &put) != 0 ||
                                     put != (uint32_t)out_len)) ||
                    vfs_close(&rf) != 0) {
                    vfs_close(&rf);
                    set_status(1);
                    console_error("Redirect failed");
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

static int eval_condition(const char *s, const char **rest_out);
static int exec_range(int lo, int hi);
static int exec_leaf(char *line);

static int cond_parse_int(const char *s, int64_t *out) {
    uint64_t u = 0;
    int neg = 0;

    if (!s) {
        return -1;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        u = u * 10ull + (uint64_t)(*s - '0');
        s++;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s) {
        return -1;
    }
    *out = neg ? -(int64_t)u : (int64_t)u;
    return 0;
}

static int cond_num(const char *tok, int64_t *out) {
    const char *v;
    const char *p;

    if (cond_parse_int(tok, out) == 0) {
        return 0;
    }
    if (!tok || !env_name_start((unsigned char)tok[0])) {
        return env_arith_eval(tok, out);
    }
    p = tok + 1;
    while (env_name_char((unsigned char)*p)) {
        p++;
    }
    if (*p) {
        return env_arith_eval(tok, out);
    }
    v = env_get(tok);
    if (!v) {
        return -1;
    }
    return cond_parse_int(v, out);
}

static int eval_condition(const char *s, const char **rest_out) {
    char left[LINE_MAX];
    char right[LINE_MAX];
    int neg = 0;
    int ok = 0;

    s = skip_spaces(s);
    if (line_kw(s, "not")) {
        neg = 1;
        s = after_kw(s, "not");
    }

    if (line_kw(s, "exist") || line_kw(s, "exists")) {
        uint32_t size = 0;
        int is_dir = 0;
        s = line_kw(s, "exists") ? after_kw(s, "exists") : after_kw(s, "exist");
        s = take_token(s, left, sizeof(left));
        ok = left[0] && vfs_stat(vfs_get_drive(), left, &size, &is_dir) == 0;
    } else if (line_kw(s, "errorlevel")) {
        s = after_kw(s, "errorlevel");
        s = take_token(s, left, sizeof(left));
        ok = last_status >= atoi(left);
    } else if (line_kw(s, "true")) {
        s = after_kw(s, "true");
        ok = 1;
    } else if (line_kw(s, "false")) {
        s = after_kw(s, "false");
        ok = 0;
    } else {
        int cop = 0;
        int64_t lv = 0;
        int64_t rv = 0;
        int ln;
        int rn;

        s = take_token(s, left, sizeof(left));
        s = skip_spaces(s);
        if (s[0] == '<' && s[1] == '=') {
            cop = 4;
            s += 2;
        } else if (s[0] == '>' && s[1] == '=') {
            cop = 6;
            s += 2;
        } else if (s[0] == '=' && s[1] == '=') {
            cop = 1;
            s += 2;
        } else if ((s[0] == '!' && s[1] == '=') || (s[0] == '<' && s[1] == '>')) {
            cop = 2;
            s += 2;
        } else if (s[0] == '<') {
            cop = 3;
            s++;
        } else if (s[0] == '>') {
            cop = 5;
            s++;
        } else if (s[0] == '=') {
            cop = 1;
            s++;
        }
        if (!cop || !left[0]) {
            if (rest_out) {
                *rest_out = skip_spaces(s);
            }
            return 0;
        }
        s = take_token(s, right, sizeof(right));
        ln = cond_num(left, &lv) == 0;
        rn = cond_num(right, &rv) == 0;
        if (cop >= 3) {
            if (!ln && env_name_start((unsigned char)left[0])) {
                lv = 0;
                ln = 1;
            }
            if (!rn && env_name_start((unsigned char)right[0])) {
                rv = 0;
                rn = 1;
            }
            if (ln && rn) {
                ok = (cop == 3) ? lv < rv :
                     (cop == 4) ? lv <= rv :
                     (cop == 5) ? lv > rv : lv >= rv;
            }
        } else if (ln && rn) {
            ok = (lv == rv);
            if (cop == 2) {
                ok = !ok;
            }
        } else {
            const char *ls = env_get(left);
            const char *rs = env_get(right);
            if (!ls) {
                ls = left;
            }
            if (!rs) {
                rs = right;
            }
            ok = strcmp(ls, rs) == 0;
            if (cop == 2) {
                ok = !ok;
            }
        }
    }

    if (neg) {
        ok = !ok;
    }
    if (rest_out) {
        *rest_out = skip_spaces(s);
    }
    return ok;
}

static int line_opens_block(const char *line) {
    const char *s = skip_spaces(line);
    const char *rest;

    if (line_kw(s, "if")) {
        s = after_kw(s, "if");
        eval_condition(s, &rest);
        rest = skip_spaces(rest);
        if (line_kw(rest, "then")) {
            rest = after_kw(rest, "then");
        }
        return *rest == 0;
    }
    if (line_kw(s, "while")) {
        s = after_kw(s, "while");
        eval_condition(s, &rest);
        rest = skip_spaces(rest);
        if (line_kw(rest, "do")) {
            rest = after_kw(rest, "do");
        }
        return *rest == 0;
    }
    if (line_kw(s, "for")) {
        rest = s;
        while (*rest) {
            if (line_kw(rest, "do")) {
                return *skip_spaces(after_kw(rest, "do")) == 0;
            }
            if (*rest == '"') {
                rest++;
                while (*rest && *rest != '"') {
                    rest++;
                }
                if (*rest == '"') {
                    rest++;
                }
                continue;
            }
            rest++;
        }
        return 1;
    }
    return 0;
}

static int line_is_end(const char *line) {
    return line_kw(line, "end") || line_kw(line, "endif") ||
           line_kw(line, "done") || line_kw(line, "wend");
}

static int find_block_end(int start, int hi, int *else_idx) {
    int i;
    int depth = 1;

    if (else_idx) {
        *else_idx = -1;
    }
    for (i = start + 1; i < hi; i++) {
        if (line_opens_block(script_lines[i])) {
            depth++;
        } else if (line_is_end(script_lines[i])) {
            depth--;
            if (depth == 0) {
                return i;
            }
        } else if (else_idx && depth == 1 && line_kw(script_lines[i], "else") &&
                   *else_idx < 0) {
            *else_idx = i;
        }
    }
    return -1;
}

static void split_else(char *s, char **else_part) {
    char q = 0;
    char *p;

    *else_part = 0;
    for (p = s; *p; p++) {
        q = quote_push(q, *p);
        if (q || *p == ' ' || *p == '\t') {
            continue;
        }
        if (!line_kw(p, "else")) {
            continue;
        }
        if (p != s && p[-1] != ' ' && p[-1] != '\t') {
            continue;
        }
        *p = 0;
        *else_part = (char *)skip_spaces(p + 4);
        trim(s);
        trim(*else_part);
        return;
    }
}

static int exec_leaf(char *line) {
    char q = 0;
    char *start = line;
    char *p;

    for (p = line;; p++) {
        char save;
        if (*p) {
            q = quote_push(q, *p);
        }
        if (*p && (q || (*p != ';' && *p != '&'))) {
            continue;
        }
        save = *p;
        *p = 0;
        trim(start);
        if (*start) {
            run_pipeline(start);
        }
        if (!save) {
            break;
        }
        start = p + 1;
    }
    return FLOW_NEXT;
}

static int exec_one(char *line);

static int run_body_text(char *body) {
    char exp[LINE_MAX];

    if (!body) {
        return FLOW_NEXT;
    }
    trim(body);
    if (*body == 0) {
        return FLOW_NEXT;
    }
    if (env_expand(body, exp, sizeof(exp)) != 0) {
        console_error("Line too long after $ expansion");
        set_status(1);
        return FLOW_NEXT;
    }
    return exec_one(exp);
}

static int exec_if_at(int i, int hi, int *next) {
    char buf[LINE_MAX];
    char *line;
    const char *rest;
    int cond;
    int else_at = -1;
    int end_at;
    char *else_part = 0;

    if (env_expand(script_lines[i], buf, sizeof(buf)) != 0) {
        console_error("Line too long after $ expansion");
        set_status(1);
        *next = i + 1;
        return FLOW_NEXT;
    }
    line = (char *)skip_spaces(buf);
    cond = eval_condition(after_kw(line, "if"), &rest);
    rest = skip_spaces(rest);
    if (line_kw(rest, "then")) {
        rest = after_kw(rest, "then");
    }
    rest = skip_spaces(rest);

    if (*rest) {
        char body[LINE_MAX];
        strncpy(body, rest, sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
        split_else(body, &else_part);
        *next = i + 1;
        if (cond) {
            return run_body_text(body);
        }
        return run_body_text(else_part);
    }

    end_at = find_block_end(i, hi, &else_at);
    if (end_at < 0) {
        console_error("if: missing end");
        set_status(1);
        *next = hi;
        return FLOW_NEXT;
    }
    *next = end_at + 1;
    if (cond) {
        int to = else_at >= 0 ? else_at : end_at;
        return exec_range(i + 1, to);
    }
    if (else_at >= 0) {
        char ebuf[LINE_MAX];
        const char *ep;
        int flow;

        if (env_expand(script_lines[else_at], ebuf, sizeof(ebuf)) != 0) {
            set_status(1);
            return FLOW_NEXT;
        }
        ep = skip_spaces(after_kw(skip_spaces(ebuf), "else"));
        if (*ep) {
            flow = run_body_text((char *)ep);
            if (flow == FLOW_BREAK || flow == FLOW_CONT) {
                return flow;
            }
        }
        return exec_range(else_at + 1, end_at);
    }
    return FLOW_NEXT;
}

static int parse_for_header(char *line, char *var, size_t var_sz,
                            int *numeric, int *from, int *to,
                            char *items, size_t items_sz, char **inline_body) {
    const char *s = after_kw(skip_spaces(line), "for");
    char tok[LINE_MAX];
    size_t n = 0;

    *numeric = 0;
    *inline_body = 0;
    items[0] = 0;
    s = take_token(s, var, var_sz);
    if (var[0] == '%') {
        memmove(var, var + 1, strlen(var));
    }
    if (var[0] == 0) {
        return -1;
    }
    s = skip_spaces(s);
    if (*s == '=') {
        char exp[LINE_MAX];
        s++;
        s = take_token(s, tok, sizeof(tok));
        if (env_expand(tok, exp, sizeof(exp)) != 0) {
            return -1;
        }
        *from = atoi(exp);
        s = skip_spaces(s);
        if (!line_kw(s, "to")) {
            return -1;
        }
        s = after_kw(s, "to");
        s = take_token(s, tok, sizeof(tok));
        if (env_expand(tok, exp, sizeof(exp)) != 0) {
            return -1;
        }
        *to = atoi(exp);
        *numeric = 1;
    } else {
        if (line_kw(s, "in")) {
            s = after_kw(s, "in");
        }
        s = skip_spaces(s);
        if (*s == '(') {
            s++;
        }
        while (*s && !line_kw(s, "do") && *s != ')') {
            s = take_token(s, tok, sizeof(tok));
            if (tok[0] == 0) {
                break;
            }
            if (n && n + 1 < items_sz) {
                items[n++] = ' ';
            }
            strncpy(items + n, tok, items_sz - n - 1);
            n = strlen(items);
            s = skip_spaces(s);
        }
        if (*s == ')') {
            s++;
        }
    }
    s = skip_spaces(s);
    if (line_kw(s, "do")) {
        s = after_kw(s, "do");
    }
    s = skip_spaces(s);
    if (*s) {
        *inline_body = (char *)s;
    }
    return 0;
}

static int exec_for_at(int i, int hi, int *next) {
    char buf[LINE_MAX];
    char var[ENV_NAME_MAX];
    char items[LINE_MAX];
    char items_exp[LINE_MAX];
    char body_copy[LINE_MAX];
    char *inline_body = 0;
    int numeric = 0;
    int from = 0;
    int to = 0;
    int end_at;
    int iters = 0;
    int flow;

    strncpy(buf, script_lines[i], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    if (parse_for_header(buf, var, sizeof(var), &numeric, &from, &to,
                         items, sizeof(items), &inline_body) != 0) {
        console_error("for: syntax is  for i in a b c do  or  for i = 1 to 10 do");
        set_status(1);
        *next = i + 1;
        return FLOW_NEXT;
    }
    if (inline_body) {
        strncpy(body_copy, inline_body, sizeof(body_copy) - 1);
        body_copy[sizeof(body_copy) - 1] = 0;
        inline_body = body_copy;
        *next = i + 1;
        end_at = -1;
    } else {
        end_at = find_block_end(i, hi, 0);
        if (end_at < 0) {
            console_error("for: missing end");
            set_status(1);
            *next = hi;
            return FLOW_NEXT;
        }
        *next = end_at + 1;
    }
    if (!numeric) {
        if (env_expand(items, items_exp, sizeof(items_exp)) != 0) {
            console_error("Line too long after $ expansion");
            set_status(1);
            return FLOW_NEXT;
        }
        strncpy(items, items_exp, sizeof(items) - 1);
        items[sizeof(items) - 1] = 0;
    }

    if (numeric) {
        int step = from <= to ? 1 : -1;
        int v;
        for (v = from; (step > 0) ? v <= to : v >= to; v += step) {
            char num[12];
            if (++iters > LOOP_MAX || keyboard_check_ctrl_c()) {
                console_write_line("^C");
                set_status(1);
                return FLOW_BREAK;
            }
            int_to_dec(v, num, sizeof(num));
            env_set(var, num);
            flow = inline_body ? run_body_text(inline_body)
                               : exec_range(i + 1, end_at);
            if (flow == FLOW_BREAK) {
                return FLOW_NEXT;
            }
        }
        return FLOW_NEXT;
    }

    {
        const char *p = items;
        char tok[LINE_MAX];
        while (*p) {
            if (++iters > LOOP_MAX || keyboard_check_ctrl_c()) {
                console_write_line("^C");
                set_status(1);
                return FLOW_BREAK;
            }
            p = take_token(p, tok, sizeof(tok));
            if (tok[0] == 0) {
                break;
            }
            env_set(var, tok);
            flow = inline_body ? run_body_text(inline_body)
                               : exec_range(i + 1, end_at);
            if (flow == FLOW_BREAK) {
                return FLOW_NEXT;
            }
        }
    }
    return FLOW_NEXT;
}

static int exec_while_at(int i, int hi, int *next) {
    char buf[LINE_MAX];
    const char *rest;
    int has_inline;
    int end_at = -1;
    int iters = 0;
    int flow;

    if (env_expand(script_lines[i], buf, sizeof(buf)) != 0) {
        console_error("Line too long after $ expansion");
        set_status(1);
        *next = i + 1;
        return FLOW_NEXT;
    }
    eval_condition(after_kw(skip_spaces(buf), "while"), &rest);
    rest = skip_spaces(rest);
    if (line_kw(rest, "do")) {
        rest = after_kw(rest, "do");
    }
    rest = skip_spaces(rest);
    has_inline = *rest != 0;

    if (has_inline) {
        *next = i + 1;
    } else {
        end_at = find_block_end(i, hi, 0);
        if (end_at < 0) {
            console_error("while: missing end");
            set_status(1);
            *next = hi;
            return FLOW_NEXT;
        }
        *next = end_at + 1;
    }

    for (;;) {
        int cond;
        char hdr[LINE_MAX];
        char body_copy[LINE_MAX];

        if (++iters > LOOP_MAX || keyboard_check_ctrl_c()) {
            console_write_line("^C");
            set_status(1);
            return FLOW_BREAK;
        }
        if (env_expand(script_lines[i], hdr, sizeof(hdr)) != 0) {
            set_status(1);
            return FLOW_NEXT;
        }
        cond = eval_condition(after_kw(skip_spaces(hdr), "while"), &rest);
        if (!cond) {
            break;
        }
        if (has_inline) {
            rest = skip_spaces(rest);
            if (line_kw(rest, "do")) {
                rest = after_kw(rest, "do");
            }
            strncpy(body_copy, skip_spaces(rest), sizeof(body_copy) - 1);
            body_copy[sizeof(body_copy) - 1] = 0;
            flow = run_body_text(body_copy);
        } else {
            flow = exec_range(i + 1, end_at);
        }
        if (flow == FLOW_BREAK) {
            break;
        }
    }
    return FLOW_NEXT;
}

static int exec_one(char *line) {
    line = (char *)skip_spaces(line);
    if (*line == 0 || *line == '#' || line_kw(line, "rem")) {
        return FLOW_NEXT;
    }
    if (line_kw(line, "break")) {
        return FLOW_BREAK;
    }
    if (line_kw(line, "continue")) {
        return FLOW_CONT;
    }
    if (line_kw(line, "if") || line_kw(line, "for") || line_kw(line, "while")) {
        char saved[LINE_MAX];
        int next = 0;
        int saved_n = script_n;
        int flow;

        strncpy(saved, script_lines[0], LINE_MAX - 1);
        saved[LINE_MAX - 1] = 0;
        strncpy(script_lines[0], line, LINE_MAX - 1);
        script_lines[0][LINE_MAX - 1] = 0;
        script_n = 1;
        if (line_kw(line, "if")) {
            flow = exec_if_at(0, 1, &next);
        } else if (line_kw(line, "for")) {
            flow = exec_for_at(0, 1, &next);
        } else {
            flow = exec_while_at(0, 1, &next);
        }
        strncpy(script_lines[0], saved, LINE_MAX - 1);
        script_lines[0][LINE_MAX - 1] = 0;
        script_n = saved_n;
        return flow;
    }
    if (line_is_end(line) || line_kw(line, "else") || line_kw(line, "then") ||
        line_kw(line, "do")) {
        console_error("Unexpected keyword");
        set_status(1);
        return FLOW_NEXT;
    }
    return exec_leaf(line);
}

static int exec_range(int lo, int hi) {
    int i = lo;

    while (i < hi) {
        char buf[LINE_MAX];
        const char *raw;
        int next;
        int flow;

        if (keyboard_check_ctrl_c()) {
            console_write_line("^C");
            set_status(1);
            return FLOW_BREAK;
        }
        {
            char *s = script_lines[i];
            while (*s == ' ' || *s == '\t') {
                s++;
            }
            if (*s == '@') {
                const char *rest = skip_spaces(s + 1);
                memmove(s, rest, strlen(rest) + 1);
            }
        }
        raw = skip_spaces(script_lines[i]);
        if (*raw == 0 || *raw == '#' || line_kw(raw, "rem") ||
            line_kw(raw, "then") || line_kw(raw, "do") || bat_echo_toggle(raw)) {
            i++;
            continue;
        }
        if (line_kw(raw, "break")) {
            return FLOW_BREAK;
        }
        if (line_kw(raw, "continue")) {
            return FLOW_CONT;
        }
        if (line_kw(raw, "if")) {
            flow = exec_if_at(i, hi, &next);
            if (flow == FLOW_BREAK || flow == FLOW_CONT) {
                return flow;
            }
            i = next;
            continue;
        }
        if (line_kw(raw, "for")) {
            flow = exec_for_at(i, hi, &next);
            if (flow == FLOW_BREAK || flow == FLOW_CONT) {
                return flow;
            }
            i = next;
            continue;
        }
        if (line_kw(raw, "while")) {
            flow = exec_while_at(i, hi, &next);
            if (flow == FLOW_BREAK || flow == FLOW_CONT) {
                return flow;
            }
            i = next;
            continue;
        }
        if (line_is_end(raw) || line_kw(raw, "else")) {
            i++;
            continue;
        }
        if (env_expand(raw, buf, sizeof(buf)) != 0) {
            console_error("Line too long after $ expansion");
            set_status(1);
            return FLOW_NEXT;
        }
        flow = exec_one(buf);
        if (flow == FLOW_BREAK || flow == FLOW_CONT) {
            return flow;
        }
        i++;
    }
    return FLOW_NEXT;
}

static void script_reset(void) {
    script_n = 0;
    script_depth = 0;
}

static void submit_line(const char *line) {
    char tmp[LINE_MAX];

    strncpy(tmp, line, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    trim(tmp);

    if (tmp[0] == '@') {
        memmove(tmp, tmp + 1, strlen(tmp + 1) + 1);
        trim(tmp);
    }

    if (script_depth == 0 && tmp[0] == 0) {
        return;
    }
    if (script_depth == 0 && (line_is_end(tmp) || line_kw(tmp, "else"))) {
        console_error("Unexpected keyword");
        set_status(1);
        return;
    }
    if (script_n >= SCRIPT_MAX) {
        console_error("Script too long (64 lines)");
        set_status(1);
        script_reset();
        return;
    }
    strncpy(script_lines[script_n], tmp, LINE_MAX - 1);
    script_lines[script_n][LINE_MAX - 1] = 0;
    script_n++;
    script_depth += line_opens_block(tmp) ? 1 : 0;
    if (line_is_end(tmp)) {
        script_depth--;
    }
    if (script_depth < 0) {
        console_error("Unexpected end");
        set_status(1);
        script_reset();
        return;
    }
    if (script_depth == 0) {
        exec_range(0, script_n);
        script_reset();
    }
}

static void line_reset(void) {
    line_len = 0;
    line_cursor = 0;
    line_buf[0] = 0;
}

static void line_redraw_tail(void) {
    /* Repaint from cursor to end of line; leave hardware cursor at line_cursor. */
    console_write(line_buf + line_cursor);
    console_putchar(' ');
    int n = line_len - line_cursor + 1;
    while (n-- > 0) {
        console_cursor_back();
    }
}

static void line_backspace(void) {
    if (line_cursor <= 0) {
        return;
    }
    if (line_cursor == line_len) {
        line_len--;
        line_buf[line_len] = 0;
        line_cursor--;
        console_backspace();
        return;
    }
    memmove(line_buf + line_cursor - 1, line_buf + line_cursor,
            (size_t)(line_len - line_cursor));
    line_len--;
    line_buf[line_len] = 0;
    line_cursor--;
    console_cursor_back();
    line_redraw_tail();
}

static void line_delete(void) {
    if (line_cursor >= line_len) {
        return;
    }
    memmove(line_buf + line_cursor, line_buf + line_cursor + 1,
            (size_t)(line_len - line_cursor - 1));
    line_len--;
    line_buf[line_len] = 0;
    line_redraw_tail();
}

static void line_insert(char c) {
    if (line_len + 1 >= LINE_MAX) {
        return;
    }
    if (line_cursor == line_len) {
        line_buf[line_len++] = c;
        line_buf[line_len] = 0;
        line_cursor++;
        console_putchar(c);
        return;
    }
    memmove(line_buf + line_cursor + 1, line_buf + line_cursor,
            (size_t)(line_len - line_cursor));
    line_buf[line_cursor] = c;
    line_len++;
    line_buf[line_len] = 0;
    /* Paint the new character and the tail; the old redraw started *after*
     * the insert point so typing in the middle (e.g. before ()) was invisible. */
    console_write(line_buf + line_cursor);
    console_putchar(' ');
    {
        int back = line_len - line_cursor;
        while (back-- > 0) {
            console_cursor_back();
        }
    }
    line_cursor++;
}

static void line_cursor_left(void) {
    if (line_cursor > 0) {
        line_cursor--;
        console_cursor_back();
    }
}

static void line_cursor_right(void) {
    if (line_cursor < line_len) {
        console_putchar(line_buf[line_cursor]);
        line_cursor++;
    }
}

static void line_replace_from(int start, const char *replacement) {
    while (line_len > start) {
        line_backspace();
    }
    if (!replacement) {
        replacement = "";
    }
    for (const char *p = replacement; *p; p++) {
        line_insert(*p);
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
        line_insert(*p);
    }
    line_cursor = line_len;
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
    /* Index of the word being edited (for Tab completion) — stops at | > space. */
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
    /* Tab: complete filename on current drive; list matches if ambiguous. */
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
    line_cursor = line_len;
}

static void shell_restore_live_view(void) {
    if (!console_at_bottom()) {
        console_scroll_to_bottom();
        print_prompt();
        console_write(line_buf);
        line_cursor = line_len;
    }
}

void shell_run(void) {
    if (shell_depth >= SHELL_NEST) {
        console_error("Too many nested shells");
        return;
    }
    shell_depth++;
    if (shell_depth == 1) {
        print_banner();
        script_reset();
        set_status(0);
    } else {
        console_write_line("Nested shell — type exit to return");
    }
    print_prompt();

    for (;;) {
        mouse_state_t m;

        (void)keyboard_has_key();
        if (mouse_get(&m) && (m.pending & MOUSE_CLICK_L)) {
            int cx;
            int cy;
            console_get_cursor(&cx, &cy);
            if (m.y == cy && m.x >= prompt_cols) {
                int dest = (int)m.x - prompt_cols;
                if (dest > line_len) {
                    dest = line_len;
                }
                if (dest < 0) {
                    dest = 0;
                }
                while (line_cursor > dest) {
                    line_cursor_left();
                }
                while (line_cursor < dest) {
                    line_cursor_right();
                }
            }
        }

        if (!keyboard_has_key()) {
            __asm__ volatile("pause");
            continue;
        }

        key_event_t ev = keyboard_read_event();
        if (ev.type == KEY_NONE) {
            continue;
        }

        if (ev.type == KEY_PAGEUP) {
            console_page_up();
            continue;
        }

        if (ev.type == KEY_PAGEDOWN) {
            console_page_down();
            continue;
        }

        shell_restore_live_view();

        if (ev.type == KEY_ENTER) {
            console_putchar('\n');
            hist_add(line_buf);
            hist_pos = -1;
            submit_line(line_buf);
            line_reset();
            if (shell_leave) {
                shell_leave = 0;
                break;
            }
            print_prompt();
            continue;
        }

        if (ev.type == KEY_BACKSPACE) {
            line_backspace();
            hist_pos = -1;
            continue;
        }

        if (ev.type == KEY_DELETE) {
            line_delete();
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

        if (ev.type == KEY_LEFT) {
            line_cursor_left();
            continue;
        }

        if (ev.type == KEY_RIGHT) {
            line_cursor_right();
            continue;
        }

        if (ev.type == KEY_CHAR && ev.ch == 3) {
            /* Ctrl+C — abandon current line and any open if/for/while. */
            console_write("^C");
            console_putchar('\n');
            line_reset();
            hist_pos = -1;
            script_reset();
            print_prompt();
            continue;
        }

        if (ev.type == KEY_CHAR && (unsigned char)ev.ch >= 32) {
            hist_pos = -1;
            line_insert(ev.ch);
        }
    }
    shell_depth--;
}
