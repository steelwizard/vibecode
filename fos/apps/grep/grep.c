/*
 * grep.c — Fixed-string line search (a simpler GNU grep -F).
 *
 *   grep [-inv] PATTERN [FILE ...]
 *   type FILE | grep PATTERN
 *
 *  -i  ignore case
 *  -n  print line numbers
 *  -v  invert match
 *
 * No FILE (or FILE is -) reads the pipe. Patterns are literal, not regex.
 */

#include "fos_api.h"

#define LINE_MAX  512
#define FILE_MAX  16
#define NAME_MAX  128
#define PAT_MAX   128

typedef struct {
    fos_api_t  *api;
    const char *pat;
    int         plen;
    int         icase;
    int         invert;
    int         numbers;
    const char *label;
    int         show_name;
    uint32_t    lineno;
    int         llen;
    char        line[LINE_MAX];
} grep_t;

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static void cpy(char *dst, size_t cap, const char *src) {
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

static int slen(const char *s) {
    int n = 0;

    while (s && s[n]) {
        n++;
    }
    return n;
}

static int streq(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Next argv-style token. "double" and 'single' quotes keep spaces. */
static const char *next_token(const char *s, char *out, size_t cap) {
    size_t n = 0;
    char q = 0;

    s = skip_ws(s);
    if (*s == '"' || *s == '\'') {
        q = *s++;
        while (*s && *s != q && n + 1 < cap) {
            out[n++] = *s++;
        }
        if (*s == q) {
            s++;
        }
    } else {
        while (*s && *s != ' ' && *s != '\t' && n + 1 < cap) {
            out[n++] = *s++;
        }
    }
    out[n] = 0;
    return s;
}

static void usage(fos_api_t *api) {
    api->write_line("grep [-inv] PATTERN [FILE ...]");
    api->write_line("  -i  ignore case   -n  line numbers   -v  invert");
    api->write_line("  Literal substring. No FILE (or -) reads the pipe.");
}

static void write_u32(fos_api_t *api, uint32_t n) {
    char buf[10];
    int i = 0;

    if (n == 0) {
        api->putchar('0');
        return;
    }
    while (n && i < 10) {
        buf[i++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    while (i--) {
        api->putchar(buf[i]);
    }
}

static char fold(char c, int icase) {
    if (icase && c >= 'A' && c <= 'Z') {
        return (char)(c + 32);
    }
    return c;
}

static int line_has(const char *line, int llen, const char *pat, int plen, int icase) {
    int i, j;

    if (plen <= 0) {
        return 1;
    }
    if (plen > llen) {
        return 0;
    }
    for (i = 0; i <= llen - plen; i++) {
        for (j = 0; j < plen; j++) {
            if (fold(line[i + j], icase) != fold(pat[j], icase)) {
                break;
            }
        }
        if (j == plen) {
            return 1;
        }
    }
    return 0;
}

static void emit_line(grep_t *g) {
    int hit;
    int n;

    if (g->llen > 0 && g->line[g->llen - 1] == '\r') {
        g->llen--;
    }
    g->lineno++;
    n = g->llen;
    hit = line_has(g->line, n, g->pat, g->plen, g->icase);
    if (g->invert) {
        hit = !hit;
    }
    if (!hit) {
        g->llen = 0;
        return;
    }
    if (g->show_name && g->label && g->label[0]) {
        g->api->write(g->label);
        g->api->putchar(':');
    }
    if (g->numbers) {
        write_u32(g->api, g->lineno);
        g->api->putchar(':');
    }
    if (n > 0) {
        g->api->write_n(g->line, (size_t)n);
    }
    g->api->putchar('\n');
    g->llen = 0;
}

static void feed(grep_t *g, const char *buf, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            emit_line(g);
            continue;
        }
        if (g->llen + 1 < LINE_MAX) {
            g->line[g->llen++] = c;
            continue;
        }
        /* Long line: search what we have, then keep this byte. */
        emit_line(g);
        g->line[g->llen++] = c;
    }
}

static void grep_reset(grep_t *g) {
    g->lineno = 0;
    g->llen = 0;
}

static void grep_finish(grep_t *g) {
    if (g->llen > 0) {
        emit_line(g);
    }
}

static void grep_buf(grep_t *g, const char *buf, uint32_t n) {
    grep_reset(g);
    feed(g, buf, n);
    grep_finish(g);
}

static int grep_file(grep_t *g, const char *path) {
    char chunk[512];
    uint32_t off = 0;
    uint32_t size = 0;
    int is_dir = 0;

    if (!g->api->stat_file || !g->api->read_at) {
        g->api->write_line("grep: file API missing");
        return -1;
    }
    if (g->api->stat_file(path, &size, &is_dir) != 0) {
        g->api->write("grep: ");
        g->api->write(path);
        g->api->write_line(": not found");
        return -1;
    }
    if (is_dir) {
        g->api->write("grep: ");
        g->api->write(path);
        g->api->write_line(": is a directory");
        return -1;
    }

    grep_reset(g);
    for (;;) {
        uint32_t got = 0;
        if (g->api->read_at(path, off, chunk, sizeof(chunk), &got) != 0) {
            g->api->write("grep: ");
            g->api->write(path);
            g->api->write_line(": read failed");
            return -1;
        }
        if (got == 0) {
            break;
        }
        feed(g, chunk, got);
        off += got;
        if (got < sizeof(chunk)) {
            break;
        }
    }
    grep_finish(g);
    return 0;
}

/* 1 = flag consumed, 2 = end of flags, 3 = help, -1 = bad, 0 = not a flag. */
static int parse_flags(const char *tok, int *icase, int *numbers, int *invert) {
    int i;

    if (!tok[0] || tok[0] != '-' || tok[1] == 0) {
        return 0;
    }
    if (streq(tok, "--")) {
        return 2;
    }
    if (streq(tok, "--help") || streq(tok, "-h") || streq(tok, "/?")) {
        return 3;
    }
    if (tok[1] == '-') {
        return -1;
    }
    for (i = 1; tok[i]; i++) {
        if (tok[i] == 'i') {
            *icase = 1;
        } else if (tok[i] == 'n') {
            *numbers = 1;
        } else if (tok[i] == 'v') {
            *invert = 1;
        } else if (tok[i] == 'h') {
            return 3;
        } else {
            return -1;
        }
    }
    return 1;
}

static int is_stdin_name(const char *name) {
    return name[0] == '-' && name[1] == 0;
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    const char *p;
    char tok[NAME_MAX];
    char pat[PAT_MAX];
    char files[FILE_MAX][NAME_MAX];
    int nfiles = 0;
    int have_pat = 0;
    int icase = 0;
    int numbers = 0;
    int invert = 0;
    int flags_done = 0;
    int i;
    int show_name;
    grep_t g;

    pat[0] = 0;
    p = api->cmdline;
    for (;;) {
        int fl;

        p = skip_ws(p);
        if (*p == 0) {
            break;
        }
        p = next_token(p, tok, sizeof(tok));
        if (streq(tok, "/?")) {
            usage(api);
            return;
        }
        if (!flags_done) {
            fl = parse_flags(tok, &icase, &numbers, &invert);
            if (fl == 1) {
                continue;
            }
            if (fl == 2) {
                flags_done = 1;
                continue;
            }
            if (fl == 3) {
                usage(api);
                return;
            }
            if (fl < 0) {
                api->write("grep: unknown option ");
                api->write_line(tok);
                usage(api);
                return;
            }
            flags_done = 1;
        }
        if (!have_pat) {
            cpy(pat, sizeof(pat), tok);
            have_pat = 1;
            continue;
        }
        if (nfiles >= FILE_MAX) {
            api->write_line("grep: too many files");
            return;
        }
        cpy(files[nfiles], sizeof(files[nfiles]), tok);
        nfiles++;
    }

    if (!have_pat) {
        usage(api);
        return;
    }
    if (nfiles == 0 && api->pipe_in_len == 0) {
        api->write_line("grep: FILE or pipe required");
        usage(api);
        return;
    }

    show_name = nfiles > 1;
    g.api = api;
    g.pat = pat;
    g.plen = slen(pat);
    g.icase = icase;
    g.invert = invert;
    g.numbers = numbers;
    g.label = 0;
    g.show_name = show_name;

    if (nfiles == 0) {
        g.label = "(standard input)";
        grep_buf(&g, api->pipe_in, (uint32_t)api->pipe_in_len);
        return;
    }

    for (i = 0; i < nfiles; i++) {
        g.label = files[i];
        if (is_stdin_name(files[i])) {
            grep_buf(&g, api->pipe_in, (uint32_t)api->pipe_in_len);
        } else {
            grep_file(&g, files[i]);
        }
    }
}
