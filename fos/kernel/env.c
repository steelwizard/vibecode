/*
 * env.c — Process environment for the shell ($PATH, $PWD, …).
 *
 * Single global table: this is a one-user OS, so everything is "exported".
 */

#include "env.h"
#include "config.h"
#include "vfs.h"
#include "console.h"
#include "string.h"

#define ENV_MAX 32

typedef struct {
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
    int  used;
} env_var_t;

static env_var_t vars[ENV_MAX];
static env_params_t params;

int env_name_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

int env_name_char(int c) {
    return env_name_start(c) || (c >= '0' && c <= '9');
}

static env_var_t *find_var(const char *name) {
    int i;

    if (!name || !name[0]) {
        return 0;
    }
    for (i = 0; i < ENV_MAX; i++) {
        if (vars[i].used && strcmp(vars[i].name, name) == 0) {
            return &vars[i];
        }
    }
    return 0;
}

const char *env_get(const char *name) {
    env_var_t *v = find_var(name);
    return v ? v->value : 0;
}

int env_set(const char *name, const char *value) {
    env_var_t *v;
    int i;

    if (!name || !env_name_start((unsigned char)name[0])) {
        return -1;
    }
    for (i = 1; name[i]; i++) {
        if (!env_name_char((unsigned char)name[i]) || i >= ENV_NAME_MAX - 1) {
            return -1;
        }
    }
    if (!value) {
        value = "";
    }

    v = find_var(name);
    if (!v) {
        for (i = 0; i < ENV_MAX; i++) {
            if (!vars[i].used) {
                v = &vars[i];
                strncpy(v->name, name, sizeof(v->name) - 1);
                v->name[sizeof(v->name) - 1] = 0;
                v->used = 1;
                break;
            }
        }
    }
    if (!v) {
        return -1;
    }
    strncpy(v->value, value, sizeof(v->value) - 1);
    v->value[sizeof(v->value) - 1] = 0;
    return 0;
}

int env_unset(const char *name) {
    env_var_t *v = find_var(name);
    if (!v) {
        return -1;
    }
    v->used = 0;
    v->name[0] = 0;
    v->value[0] = 0;
    return 0;
}

void env_sync_pwd(void) {
    char pwd[VFS_PATH_MAX];
    char drive[4];
    const char *cwd;
    int d;
    size_t n;

    d = vfs_get_drive();
    cwd = vfs_get_cwd();
    if (!cwd || !cwd[0]) {
        cwd = "\\";
    }

    pwd[0] = (char)('0' + d);
    pwd[1] = ':';
    n = 2;
    while (*cwd && n + 1 < sizeof(pwd)) {
        pwd[n++] = *cwd++;
    }
    pwd[n] = 0;
    env_set("PWD", pwd);

    drive[0] = (char)('0' + d);
    drive[1] = 0;
    env_set("DRIVE", drive);
}

void env_params_save(env_params_t *out) {
    if (out) {
        memcpy(out, &params, sizeof(params));
    }
}

void env_params_load(const env_params_t *in) {
    if (in) {
        memcpy(&params, in, sizeof(params));
    } else {
        memset(&params, 0, sizeof(params));
    }
}

void env_set_params(const char *arg0, const char *args) {
    const char *p;
    int i;

    memset(&params, 0, sizeof(params));
    if (arg0) {
        strncpy(params.arg[0], arg0, sizeof(params.arg[0]) - 1);
        params.arg[0][sizeof(params.arg[0]) - 1] = 0;
    }
    if (!args) {
        return;
    }
    strncpy(params.all, args, sizeof(params.all) - 1);
    params.all[sizeof(params.all) - 1] = 0;
    p = skip_spaces(args);
    for (i = 1; i < 10 && *p; i++) {
        size_t n = 0;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q && n + 1 < sizeof(params.arg[0])) {
                params.arg[i][n++] = *p++;
            }
            if (*p == q) {
                p++;
            }
        } else {
            while (*p && *p != ' ' && *p != '\t' && n + 1 < sizeof(params.arg[0])) {
                params.arg[i][n++] = *p++;
            }
        }
        params.arg[i][n] = 0;
        p = skip_spaces(p);
    }
}

void env_init(void) {
    const char *path;

    memset(vars, 0, sizeof(vars));
    memset(&params, 0, sizeof(params));
    path = config_get("shell", "path");
    env_set("PATH", (path && path[0]) ? path : "\\FOS:\\GAMES");
    env_set("HOME", "\\");
    env_set("ERRORLEVEL", "0");
    env_sync_pwd();
}

void env_print(void) {
    int i;
    for (i = 0; i < ENV_MAX; i++) {
        if (!vars[i].used) {
            continue;
        }
        console_write(vars[i].name);
        console_putchar('=');
        console_write_line(vars[i].value);
    }
}

typedef struct {
    const char *p;
    int         err;
} arith_t;

static void arith_skip(arith_t *a) {
    while (*a->p == ' ' || *a->p == '\t') {
        a->p++;
    }
}

static int64_t arith_expr(arith_t *a);
static void i64_dec(int64_t v, char *buf);

static int64_t arith_num(const char *s, int *ok) {
    uint64_t u = 0;
    int neg = 0;

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
        if (ok) {
            *ok = 0;
        }
        return 0;
    }
    while (*s >= '0' && *s <= '9') {
        u = u * 10ull + (uint64_t)(*s - '0');
        s++;
    }
    if (ok) {
        *ok = 1;
    }
    return neg ? -(int64_t)u : (int64_t)u;
}

/* Whole string is an integer (optional sign/whitespace). Empty/NULL → 0. */
static int i64_from_str(const char *s, int64_t *out) {
    const char *p;
    int ok = 0;
    int64_t v;

    if (!s) {
        *out = 0;
        return 0;
    }
    p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == 0) {
        *out = 0;
        return 0;
    }
    v = arith_num(p, &ok);
    if (!ok) {
        return -1;
    }
    if (*p == '+' || *p == '-') {
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p) {
        return -1;
    }
    *out = v;
    return 0;
}

static int64_t arith_primary(arith_t *a) {
    int64_t v;

    arith_skip(a);
    if (*a->p == '(') {
        a->p++;
        v = arith_expr(a);
        arith_skip(a);
        if (*a->p == ')') {
            a->p++;
        } else {
            a->err = 1;
        }
        return v;
    }
    if (*a->p == '-' || *a->p == '+') {
        int neg = (*a->p++ == '-');
        v = arith_primary(a);
        return neg ? -v : v;
    }
    if (*a->p == '$') {
        char nbuf[ENV_NAME_MAX];
        size_t nl = 0;
        const char *val;

        a->p++;
        if (*a->p == '(') {
            return arith_primary(a);
        }
        if (*a->p == '{') {
            a->p++;
            while (*a->p && *a->p != '}' && nl + 1 < sizeof(nbuf)) {
                nbuf[nl++] = *a->p++;
            }
            nbuf[nl] = 0;
            if (*a->p == '}') {
                a->p++;
            }
        } else if (env_name_start((unsigned char)*a->p)) {
            while (env_name_char((unsigned char)*a->p) && nl + 1 < sizeof(nbuf)) {
                nbuf[nl++] = *a->p++;
            }
            nbuf[nl] = 0;
        } else {
            a->err = 1;
            return 0;
        }
        val = env_get(nbuf);
        if (i64_from_str(val, &v) != 0) {
            a->err = 1;
            return 0;
        }
        return v;
    }
    if (*a->p >= '0' && *a->p <= '9') {
        int ok = 0;
        v = arith_num(a->p, &ok);
        while (*a->p >= '0' && *a->p <= '9') {
            a->p++;
        }
        return v;
    }
    if (env_name_start((unsigned char)*a->p)) {
        char nbuf[ENV_NAME_MAX];
        size_t nl = 0;
        const char *val;

        while (env_name_char((unsigned char)*a->p) && nl + 1 < sizeof(nbuf)) {
            nbuf[nl++] = *a->p++;
        }
        nbuf[nl] = 0;
        val = env_get(nbuf);
        if (i64_from_str(val, &v) != 0) {
            a->err = 1;
            return 0;
        }
        return v;
    }
    a->err = 1;
    return 0;
}

static int64_t arith_term(arith_t *a) {
    int64_t v = arith_primary(a);

    for (;;) {
        arith_skip(a);
        if (*a->p == '*') {
            a->p++;
            v = v * arith_primary(a);
        } else if (*a->p == '/') {
            int64_t r;
            a->p++;
            r = arith_primary(a);
            if (r == 0) {
                a->err = 1;
                return 0;
            }
            v = v / r;
        } else if (*a->p == '%') {
            int64_t r;
            a->p++;
            r = arith_primary(a);
            if (r == 0) {
                a->err = 1;
                return 0;
            }
            v = v % r;
        } else {
            return v;
        }
    }
}

static int64_t arith_expr(arith_t *a) {
    int64_t v = arith_term(a);

    for (;;) {
        arith_skip(a);
        if (*a->p == '+') {
            a->p++;
            v = v + arith_term(a);
        } else if (*a->p == '-') {
            a->p++;
            v = v - arith_term(a);
        } else {
            return v;
        }
    }
}

static int arith_eval(const char *s, int64_t *out) {
    arith_t a;

    a.p = s;
    a.err = 0;
    *out = arith_expr(&a);
    arith_skip(&a);
    if (a.err || *a.p) {
        return -1;
    }
    return 0;
}

int env_arith_eval(const char *expr, int64_t *out) {
    if (!expr || !out) {
        return -1;
    }
    return arith_eval(expr, out);
}

/*
 * True when `s` is worth evaluating as integer math for NAME=s:
 * a variable plus an operator (i=i+1), or + * / % / parentheses (5+1, 2*3).
 * Bare minus (2020-01-01) stays a string.
 */
static int arith_looks_expr(const char *s) {
    int has_name = 0;
    int has_pm = 0;
    int has_op = 0;
    int has_paren = 0;

    if (!s) {
        return 0;
    }
    while (*s) {
        if (*s == ' ' || *s == '\t') {
            s++;
            continue;
        }
        if (env_name_start((unsigned char)*s)) {
            has_name = 1;
            while (env_name_char((unsigned char)*s)) {
                s++;
            }
            continue;
        }
        if (*s >= '0' && *s <= '9') {
            while (*s >= '0' && *s <= '9') {
                s++;
            }
            continue;
        }
        if (*s == '+' || *s == '*' || *s == '/' || *s == '%') {
            has_pm = 1;
            has_op = 1;
            s++;
            continue;
        }
        if (*s == '-') {
            has_op = 1;
            s++;
            continue;
        }
        if (*s == '(' || *s == ')') {
            has_paren = 1;
            s++;
            continue;
        }
        return 0;
    }
    return (has_name && has_op) || has_pm || has_paren;
}

int env_try_arith(const char *expr, int64_t *out) {
    if (!arith_looks_expr(expr)) {
        return -1;
    }
    return arith_eval(expr, out);
}

int env_get_i64(const char *name, int64_t *out) {
    return i64_from_str(env_get(name), out);
}

int env_set_i64(const char *name, int64_t v) {
    char buf[24];

    i64_dec(v, buf);
    return env_set(name, buf);
}

static void i64_dec(int64_t v, char *buf) {
    char tmp[24];
    uint64_t u;
    int n = 0;
    int i = 0;
    int neg = v < 0;

    if (!neg) {
        u = (uint64_t)v;
    } else {
        u = (uint64_t)0 - (uint64_t)v;
    }
    if (u == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (u) {
        tmp[n++] = (char)('0' + (u % 10ull));
        u /= 10ull;
    }
    if (neg) {
        buf[i++] = '-';
    }
    while (n--) {
        buf[i++] = tmp[n];
    }
    buf[i] = 0;
}

static int copy_out(char *out, size_t cap, size_t *o, const char *s) {
    if (!s) {
        return 0;
    }
    while (*s) {
        if (*o + 1 >= cap) {
            out[cap - 1] = 0;
            return -1;
        }
        out[(*o)++] = *s++;
    }
    return 0;
}

int env_expand(const char *in, char *out, size_t cap) {
    size_t o = 0;
    char quote = 0;

    if (!in || !out || cap == 0) {
        return -1;
    }

    while (*in) {
        if (!quote && *in == '\\' && in[1] == '$') {
            if (o + 1 >= cap) {
                goto overflow;
            }
            out[o++] = '$';
            in += 2;
            continue;
        }

        if (*in == '"' || *in == '\'') {
            if (!quote) {
                quote = *in;
            } else if (quote == *in) {
                quote = 0;
            }
            if (o + 1 >= cap) {
                goto overflow;
            }
            out[o++] = *in++;
            continue;
        }

        if (*in == '$' && quote != '\'') {
            const char *name;
            char nbuf[ENV_NAME_MAX];
            size_t nl = 0;
            const char *val;

            in++;
            if (*in == '(') {
                char body[ENV_VALUE_MAX];
                size_t bl = 0;
                int depth = 1;
                int64_t n = 0;
                char num[24];

                in++;
                while (*in && depth) {
                    if (*in == '(') {
                        depth++;
                    } else if (*in == ')') {
                        depth--;
                        if (depth == 0) {
                            break;
                        }
                    }
                    if (bl + 1 < sizeof(body)) {
                        body[bl++] = *in;
                    }
                    in++;
                }
                body[bl] = 0;
                if (*in == ')') {
                    in++;
                }
                if (arith_eval(body, &n) != 0) {
                    console_error("bad arithmetic");
                    continue;
                }
                i64_dec(n, num);
                if (copy_out(out, cap, &o, num) != 0) {
                    return -1;
                }
                continue;
            }
            if (*in == '{') {
                in++;
                while (*in && *in != '}' && nl + 1 < sizeof(nbuf)) {
                    nbuf[nl++] = *in++;
                }
                nbuf[nl] = 0;
                if (*in == '}') {
                    in++;
                }
                name = nbuf;
            } else if (env_name_start((unsigned char)*in)) {
                while (env_name_char((unsigned char)*in) && nl + 1 < sizeof(nbuf)) {
                    nbuf[nl++] = *in++;
                }
                nbuf[nl] = 0;
                name = nbuf;
            } else {
                if (o + 1 >= cap) {
                    goto overflow;
                }
                out[o++] = '$';
                continue;
            }

            val = env_get(name);
            if (copy_out(out, cap, &o, val) != 0) {
                return -1;
            }
            continue;
        }

        if (*in == '%' && quote != '\'') {
            in++;
            if (*in == '%') {
                if (o + 1 >= cap) {
                    goto overflow;
                }
                out[o++] = '%';
                in++;
                continue;
            }
            if (*in == '*') {
                in++;
                if (copy_out(out, cap, &o, params.all) != 0) {
                    return -1;
                }
                continue;
            }
            if (*in >= '0' && *in <= '9') {
                int idx = *in++ - '0';
                if (copy_out(out, cap, &o, params.arg[idx]) != 0) {
                    return -1;
                }
                continue;
            }
            if (env_name_start((unsigned char)*in)) {
                char nbuf[ENV_NAME_MAX];
                size_t nl = 0;
                const char *val;

                while (env_name_char((unsigned char)*in) && nl + 1 < sizeof(nbuf)) {
                    nbuf[nl++] = *in++;
                }
                nbuf[nl] = 0;
                if (*in == '%') {
                    in++;
                    val = env_get(nbuf);
                    if (copy_out(out, cap, &o, val) != 0) {
                        return -1;
                    }
                    continue;
                }
                /* Unclosed %NAME — keep the text. */
                if (o + 1 >= cap) {
                    goto overflow;
                }
                out[o++] = '%';
                if (copy_out(out, cap, &o, nbuf) != 0) {
                    return -1;
                }
                continue;
            }
            if (o + 1 >= cap) {
                goto overflow;
            }
            out[o++] = '%';
            continue;
        }

        if (o + 1 >= cap) {
            goto overflow;
        }
        out[o++] = *in++;
    }

    out[o] = 0;
    return 0;

overflow:
    out[cap - 1] = 0;
    return -1;
}
