/*
 * foslibc.c — C library for FOS .COM programs and for TinyCC itself.
 * Talks to the kernel only through the fixed API block at FOS_API_ADDR.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "fos_api.h"

#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

int errno = 0;

jmp_buf fos_exit_buf;
int fos_exit_ready;

static fos_api_t *fosapi(void) {
    return (fos_api_t *)FOS_API_ADDR;
}

/* ---------------------------------------------------------------- memory */

void *malloc(size_t n) {
    if (n == 0) {
        n = 1;
    }
    return fosapi()->mem_alloc(n);
}

void free(void *p) {
    fosapi()->mem_free(p);
}

void *realloc(void *p, size_t n) {
    if (n == 0) {
        free(p);
        return NULL;
    }
    if (!p) {
        return malloc(n);
    }
    return fosapi()->mem_realloc(p, n);
}

void *calloc(size_t n, size_t size) {
    size_t bytes;
    void *p;
    if (n != 0 && size > (size_t)-1 / n) {
        errno = ENOMEM;
        return NULL;
    }
    bytes = n * size;
    p = malloc(bytes);
    if (p) {
        memset(p, 0, bytes);
    }
    return p;
}

/* ---------------------------------------------------------------- string */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    strcpy(d, src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i]; i++) {
        d[i] = src[i];
    }
    d[i] = 0;
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) {
            last = s;
        }
        s++;
    }
    if (c == 0) {
        return (char *)s;
    }
    return (char *)last;
}

char *strstr(const char *s, const char *n) {
    size_t ln;
    if (!n[0]) {
        return (char *)s;
    }
    ln = strlen(n);
    for (; *s; s++) {
        if (*s == *n && strncmp(s, n, ln) == 0) {
            return (char *)s;
        }
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        if (strchr(accept, *s)) {
            return (char *)s;
        }
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

char *strerror(int err) {
    (void)err;
    return "error";
}

/* ---------------------------------------------------------------- ctype */

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int isupper(int c) {
    return c >= 'A' && c <= 'Z';
}

int islower(int c) {
    return c >= 'a' && c <= 'z';
}

int tolower(int c) {
    return isupper(c) ? c - 'A' + 'a' : c;
}

int toupper(int c) {
    return islower(c) ? c - 'a' + 'A' : c;
}

/* ---------------------------------------------------------------- files */

#define LFD_MAX 40

enum { KIND_NONE = 0, KIND_KERN, KIND_IN, KIND_OUT, KIND_ERR };

typedef struct {
    int used;
    int kind;
    int kfd;
} lfd_t;

static lfd_t lfds[LFD_MAX];
static FILE stdio_store[3];
static int stdio_ready;
static size_t pipe_pos;

FILE *stdin = &stdio_store[0];
FILE *stdout = &stdio_store[1];
FILE *stderr = &stdio_store[2];

static void stdio_init(void) {
    int i;
    if (stdio_ready) {
        return;
    }
    stdio_ready = 1;
    for (i = 0; i < 3; i++) {
        stdio_store[i].fd = i;
        lfds[i].used = 1;
        lfds[i].kfd = -1;
    }
    lfds[0].kind = KIND_IN;
    lfds[1].kind = KIND_OUT;
    lfds[2].kind = KIND_ERR;
}

static int alloc_fd(void) {
    int i;
    stdio_init();
    for (i = 3; i < LFD_MAX; i++) {
        if (!lfds[i].used) {
            lfds[i].used = 1;
            lfds[i].kind = KIND_KERN;
            lfds[i].kfd = -1;
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

int open(const char *path, int flags, ...) {
    int acc = flags & 3;
    int k = 0;
    int fd;
    int kfd;
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (acc == O_WRONLY) {
        k = FOS_O_WRITE;
    } else if (acc == O_RDWR) {
        k = FOS_O_RDWR;
    } else {
        k = FOS_O_READ;
    }
    if (flags & O_CREAT) {
        k |= FOS_O_CREATE;
    }
    if (flags & O_TRUNC) {
        k |= FOS_O_TRUNC;
    }
    if (flags & O_APPEND) {
        k |= FOS_O_APPEND;
    }
    fd = alloc_fd();
    if (fd < 0) {
        return -1;
    }
    kfd = fosapi()->fopen(path, k);
    if (kfd < 0) {
        lfds[fd].used = 0;
        errno = ENOENT;
        return -1;
    }
    lfds[fd].kfd = kfd;
    return fd;
}

int close(int fd) {
    stdio_init();
    if (fd < 0 || fd >= LFD_MAX || !lfds[fd].used) {
        errno = EBADF;
        return -1;
    }
    if (fd <= 2) {
        return 0;
    }
    if (lfds[fd].kfd >= 0) {
        fosapi()->fclose(lfds[fd].kfd);
    }
    lfds[fd].used = 0;
    lfds[fd].kfd = -1;
    return 0;
}

ssize_t read(int fd, void *buf, size_t n) {
    uint32_t got = 0;
    stdio_init();
    if (fd < 0 || fd >= LFD_MAX || !lfds[fd].used || !buf) {
        errno = EBADF;
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (lfds[fd].kind == KIND_IN) {
        fos_api_t *api = fosapi();
        size_t left = api->pipe_in_len > pipe_pos ? api->pipe_in_len - pipe_pos : 0;
        if (left > n) {
            left = n;
        }
        if (left) {
            memcpy(buf, api->pipe_in + pipe_pos, left);
            pipe_pos += left;
        }
        return (ssize_t)left;
    }
    if (lfds[fd].kind != KIND_KERN || lfds[fd].kfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (n > 0xffffffffu) {
        n = 0xffffffffu;
    }
    if (fosapi()->fread(lfds[fd].kfd, buf, (uint32_t)n, &got) != 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

ssize_t write(int fd, const void *buf, size_t n) {
    uint32_t got = 0;
    stdio_init();
    if (fd < 0 || fd >= LFD_MAX || !lfds[fd].used || !buf) {
        errno = EBADF;
        return -1;
    }
    if (lfds[fd].kind == KIND_OUT || lfds[fd].kind == KIND_ERR) {
        fosapi()->write_n(buf, n);
        return (ssize_t)n;
    }
    if (lfds[fd].kind != KIND_KERN || lfds[fd].kfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (n > 0xffffffffu) {
        n = 0xffffffffu;
    }
    if (fosapi()->fwrite(lfds[fd].kfd, buf, (uint32_t)n, &got) != 0) {
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

off_t lseek(int fd, off_t off, int whence) {
    uint32_t cur = 0;
    uint32_t sz = 0;
    long neu;
    stdio_init();
    if (fd < 3 || fd >= LFD_MAX || !lfds[fd].used || lfds[fd].kfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fosapi()->ftell(lfds[fd].kfd, &cur) != 0) {
        errno = EIO;
        return -1;
    }
    if (whence == SEEK_SET) {
        neu = off;
    } else if (whence == SEEK_CUR) {
        neu = (long)cur + off;
    } else if (whence == SEEK_END) {
        if (fosapi()->fsize(lfds[fd].kfd, &sz) != 0) {
            errno = EIO;
            return -1;
        }
        neu = (long)sz + off;
    } else {
        errno = EINVAL;
        return -1;
    }
    if (neu < 0) {
        errno = EINVAL;
        return -1;
    }
    if (fosapi()->fseek(lfds[fd].kfd, (uint32_t)neu) != 0) {
        errno = EIO;
        return -1;
    }
    return neu;
}

int unlink(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (fosapi()->delete_file(path) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int remove(const char *path) {
    return unlink(path);
}

int execvp(const char *file, char *const argv[]) {
    (void)file;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

int isatty(int fd) {
    stdio_init();
    return fd >= 0 && fd <= 2 && lfds[fd].used;
}

char *getcwd(char *buf, size_t size) {
    if (size == 0) {
        size = 260;
    }
    if (!buf) {
        buf = malloc(size);
        if (!buf) {
            errno = ENOMEM;
            return NULL;
        }
    }
    fosapi()->get_cwd(buf, size);
    return buf;
}

/* ---------------------------------------------------------------- stdio */

static int mode_flags(const char *mode) {
    int plus;
    if (!mode || !mode[0]) {
        return -1;
    }
    plus = strchr(mode, '+') != NULL;
    if (mode[0] == 'r') {
        return plus ? O_RDWR : O_RDONLY;
    }
    if (mode[0] == 'w') {
        return O_WRONLY | O_CREAT | O_TRUNC | (plus ? O_RDWR : 0);
    }
    if (mode[0] == 'a') {
        return O_WRONLY | O_CREAT | O_APPEND | (plus ? O_RDWR : 0);
    }
    return -1;
}

FILE *fdopen(int fd, const char *mode) {
    FILE *f;
    (void)mode;
    stdio_init();
    if (fd < 0 || fd >= LFD_MAX || !lfds[fd].used) {
        errno = EBADF;
        return NULL;
    }
    f = malloc(sizeof *f);
    if (!f) {
        errno = ENOMEM;
        return NULL;
    }
    memset(f, 0, sizeof *f);
    f->fd = fd;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = mode_flags(mode);
    int fd;
    FILE *f;
    if (flags < 0) {
        errno = EINVAL;
        return NULL;
    }
    /* "w+" sets O_WRONLY then O_RDWR; the low bits must be exactly one of them. */
    if ((flags & O_RDWR) == O_RDWR) {
        flags = (flags & ~3) | O_RDWR;
    }
    fd = open(path, flags, 0666);
    if (fd < 0) {
        return NULL;
    }
    f = fdopen(fd, mode);
    if (!f) {
        close(fd);
    }
    return f;
}

int fclose(FILE *f) {
    int fd;
    int rc;
    if (!f) {
        errno = EBADF;
        return EOF;
    }
    if (f == stdin || f == stdout || f == stderr) {
        return 0;
    }
    fd = f->fd;
    rc = close(fd);
    free(f);
    return rc == 0 ? 0 : EOF;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int fileno(FILE *f) {
    if (!f) {
        errno = EBADF;
        return -1;
    }
    return f->fd;
}

int fgetc(FILE *f) {
    unsigned char c;
    ssize_t n;
    stdio_init();
    if (!f) {
        return EOF;
    }
    if (f->has_un) {
        f->has_un = 0;
        return f->ungot & 0xff;
    }
    n = read(f->fd, &c, 1);
    if (n <= 0) {
        f->eof = 1;
        if (n < 0) {
            f->err = 1;
        }
        return EOF;
    }
    return c;
}

int ungetc(int c, FILE *f) {
    if (!f || c == EOF || f->has_un) {
        return EOF;
    }
    f->has_un = 1;
    f->ungot = c & 0xff;
    f->eof = 0;
    return f->ungot;
}

int fputc(int c, FILE *f) {
    unsigned char b = (unsigned char)c;
    stdio_init();
    if (!f || write(f->fd, &b, 1) != 1) {
        if (f) {
            f->err = 1;
        }
        return EOF;
    }
    return b;
}

int fputs(const char *s, FILE *f) {
    size_t n;
    if (!s || !f) {
        return EOF;
    }
    n = strlen(s);
    if (n && fwrite(s, 1, n, f) != n) {
        return EOF;
    }
    return 0;
}

char *fgets(char *s, int size, FILE *f) {
    int i = 0;
    if (!s || size <= 0 || !f) {
        return NULL;
    }
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    s[i] = 0;
    return i ? s : NULL;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    unsigned char *p = ptr;
    size_t bytes;
    size_t got = 0;
    stdio_init();
    if (!f || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    if (nmemb > (size_t)-1 / size) {
        return 0;
    }
    bytes = size * nmemb;
    if (f->has_un && bytes) {
        p[got++] = (unsigned char)f->ungot;
        f->has_un = 0;
    }
    while (got < bytes) {
        ssize_t n = read(f->fd, p + got, bytes - got);
        if (n < 0) {
            f->err = 1;
            break;
        }
        if (n == 0) {
            f->eof = 1;
            break;
        }
        got += (size_t)n;
    }
    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    const unsigned char *p = ptr;
    size_t bytes;
    size_t put = 0;
    stdio_init();
    if (!f || !ptr || size == 0 || nmemb == 0) {
        return 0;
    }
    if (nmemb > (size_t)-1 / size) {
        return 0;
    }
    bytes = size * nmemb;
    while (put < bytes) {
        ssize_t n = write(f->fd, p + put, bytes - put);
        if (n <= 0) {
            f->err = 1;
            break;
        }
        put += (size_t)n;
    }
    return put / size;
}

int feof(FILE *f) {
    return f && f->eof;
}

int ferror(FILE *f) {
    return f && f->err;
}

void clearerr(FILE *f) {
    if (f) {
        f->eof = 0;
        f->err = 0;
    }
}

int fseek(FILE *f, long off, int whence) {
    stdio_init();
    if (!f || lseek(f->fd, off, whence) < 0) {
        return -1;
    }
    f->eof = 0;
    f->has_un = 0;
    return 0;
}

long ftell(FILE *f) {
    uint32_t cur = 0;
    if (!f || f->fd < 3 || lfds[f->fd].kfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (fosapi()->ftell(lfds[f->fd].kfd, &cur) != 0) {
        errno = EIO;
        return -1;
    }
    return (long)cur;
}

void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

int putchar(int c) {
    stdio_init();
    return fputc(c, stdout);
}

int getchar(void) {
    stdio_init();
    return fgetc(stdin);
}

int puts(const char *s) {
    stdio_init();
    if (fputs(s ? s : "", stdout) == EOF || fputc('\n', stdout) == EOF) {
        return EOF;
    }
    return 0;
}

/* ---------------------------------------------------------------- printf */

static void out_char(char **p, char *end, int unlimited, int *total, char c) {
    if (unlimited || (*p && *p + 1 < end)) {
        if (*p) {
            *(*p)++ = c;
        }
    }
    (*total)++;
}

static void out_repeat(char **p, char *end, int unlimited, int *total, char c, int n) {
    while (n-- > 0) {
        out_char(p, end, unlimited, total, c);
    }
}

static void out_pad(char **p, char *end, int unlimited, int *total,
                    const char *s, int len, int width, int left, char pad) {
    int padn = width > len ? width - len : 0;
    if (!left) {
        out_repeat(p, end, unlimited, total, pad, padn);
    }
    while (len-- > 0) {
        out_char(p, end, unlimited, total, *s++);
    }
    if (left) {
        out_repeat(p, end, unlimited, total, ' ', padn);
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    int unlimited = (n == (size_t)-1);
    char *p = buf;
    char *end = (!unlimited && buf && n) ? buf + n : buf;
    int total = 0;

    if (!fmt) {
        fmt = "";
    }
    while (*fmt) {
        if (*fmt != '%') {
            out_char(&p, end, unlimited, &total, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            out_char(&p, end, unlimited, &total, '%');
            fmt++;
            continue;
        }
        {
            int left = 0;
            int zero = 0;
            int width = 0;
            int prec = -1;
            int is_long = 0;
            int is_ll = 0;
            char spec;
            if (*fmt == '-') {
                left = 1;
                fmt++;
            }
            if (*fmt == '0') {
                zero = 1;
                fmt++;
            }
            if (*fmt == '*') {
                width = va_arg(ap, int);
                fmt++;
                if (width < 0) {
                    left = 1;
                    width = -width;
                }
            } else {
                while (isdigit((unsigned char)*fmt)) {
                    width = width * 10 + (*fmt++ - '0');
                }
            }
            if (*fmt == '.') {
                fmt++;
                prec = 0;
                if (*fmt == '*') {
                    prec = va_arg(ap, int);
                    fmt++;
                } else {
                    while (isdigit((unsigned char)*fmt)) {
                        prec = prec * 10 + (*fmt++ - '0');
                    }
                }
            }
            if (*fmt == 'l') {
                fmt++;
                is_long = 1;
                if (*fmt == 'l') {
                    fmt++;
                    is_ll = 1;
                }
            } else if (*fmt == 'z') {
                fmt++;
                is_long = 1;
            }
            spec = *fmt ? *fmt++ : 0;
            if (spec == 'c') {
                char ch = (char)va_arg(ap, int);
                out_pad(&p, end, unlimited, &total, &ch, 1, width, left, ' ');
            } else if (spec == 's') {
                const char *s = va_arg(ap, const char *);
                int len;
                if (!s) {
                    s = "(null)";
                }
                len = (int)strlen(s);
                if (prec >= 0 && prec < len) {
                    len = prec;
                }
                out_pad(&p, end, unlimited, &total, s, len, width, left, ' ');
            } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' ||
                       spec == 'X' || spec == 'o' || spec == 'p') {
                int neg = 0;
                int base = 10;
                int up = spec == 'X';
                char tmp[64];
                char num[64];
                int ni = 0;
                int ti = 0;
                unsigned long long v;
                char padc = (!left && zero && prec < 0) ? '0' : ' ';
                if (spec == 'p') {
                    v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
                    base = 16;
                    num[ni++] = '0';
                    num[ni++] = 'x';
                } else if (spec == 'd' || spec == 'i') {
                    long long sv;
                    if (is_ll) {
                        sv = va_arg(ap, long long);
                    } else if (is_long) {
                        sv = va_arg(ap, long);
                    } else {
                        sv = va_arg(ap, int);
                    }
                    if (sv < 0) {
                        neg = 1;
                        v = (unsigned long long)0 - (unsigned long long)sv;
                    } else {
                        v = (unsigned long long)sv;
                    }
                } else {
                    if (spec == 'x' || spec == 'X') {
                        base = 16;
                    } else if (spec == 'o') {
                        base = 8;
                    }
                    if (is_ll) {
                        v = va_arg(ap, unsigned long long);
                    } else if (is_long) {
                        v = va_arg(ap, unsigned long);
                    } else {
                        v = va_arg(ap, unsigned int);
                    }
                }
                if (v == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (v) {
                        unsigned d = (unsigned)(v % (unsigned)base);
                        tmp[ti++] = (char)(d < 10 ? '0' + d : (up ? 'A' : 'a') + (d - 10));
                        v /= (unsigned)base;
                    }
                }
                if (neg) {
                    num[ni++] = '-';
                }
                while (ti > 0) {
                    num[ni++] = tmp[--ti];
                }
                num[ni] = 0;
                out_pad(&p, end, unlimited, &total, num, ni, width, left, padc);
            } else if (spec == 'f' || spec == 'g' || spec == 'e') {
                double d = va_arg(ap, double);
                char num[64];
                int ni = 0;
                int i;
                long ip;
                double frac;
                if (d < 0) {
                    num[ni++] = '-';
                    d = -d;
                }
                ip = (long)d;
                frac = d - (double)ip;
                {
                    char rev[32];
                    int r = 0;
                    if (ip == 0) {
                        rev[r++] = '0';
                    }
                    while (ip) {
                        rev[r++] = (char)('0' + (ip % 10));
                        ip /= 10;
                    }
                    while (r > 0) {
                        num[ni++] = rev[--r];
                    }
                }
                num[ni++] = '.';
                for (i = 0; i < 6; i++) {
                    int digit;
                    frac *= 10.0;
                    digit = (int)frac;
                    num[ni++] = (char)('0' + digit);
                    frac -= digit;
                }
                num[ni] = 0;
                out_pad(&p, end, unlimited, &total, num, ni, width, left, ' ');
            } else if (spec) {
                out_char(&p, end, unlimited, &total, spec);
            }
        }
    }
    if (buf && (unlimited || n > 0)) {
        if (unlimited || p < buf + n) {
            if (p) {
                *p = 0;
            }
        } else if (n > 0) {
            buf[n - 1] = 0;
        }
    }
    return total;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char stack[2048];
    va_list aq;
    int n;
    stdio_init();
    if (!f) {
        return -1;
    }
    va_copy(aq, ap);
    n = vsnprintf(stack, sizeof stack, fmt, ap);
    if (n < 0) {
        va_end(aq);
        return -1;
    }
    if (n < (int)sizeof stack) {
        va_end(aq);
        if (n > 0 && fwrite(stack, 1, (size_t)n, f) != (size_t)n) {
            return -1;
        }
        return n;
    }
    {
        char *big = malloc((size_t)n + 1);
        if (!big) {
            fwrite(stack, 1, sizeof stack - 1, f);
            va_end(aq);
            return (int)sizeof stack - 1;
        }
        vsnprintf(big, (size_t)n + 1, fmt, aq);
        va_end(aq);
        fwrite(big, 1, (size_t)n, f);
        free(big);
        return n;
    }
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    stdio_init();
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    int r;
    stdio_init();
    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char *s) {
    stdio_init();
    if (s && s[0]) {
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    } else {
        fprintf(stderr, "%s\n", strerror(errno));
    }
}

/* ---------------------------------------------------------------- convert */

static const char *skip_ws(const char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static unsigned long long parse_ull(const char *s, char **end, int base, int *neg) {
    const char *start;
    unsigned long long v = 0;
    int any = 0;
    int sign = 1;
    s = skip_ws(s);
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    start = s;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
        s++;
    }
    if (neg) {
        *neg = sign < 0;
    }
    if (end) {
        *end = (char *)(any ? s : start);
    }
    return v;
}

long strtol(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = parse_ull(s, end, base, &neg);
    if (neg) {
        return -(long)v;
    }
    return (long)v;
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)parse_ull(s, end, base, NULL);
}

long long strtoll(const char *s, char **end, int base) {
    int neg = 0;
    unsigned long long v = parse_ull(s, end, base, &neg);
    if (neg) {
        return -(long long)v;
    }
    return (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return parse_ull(s, end, base, NULL);
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

int abs(int n) {
    return n < 0 ? -n : n;
}

double strtod(const char *nptr, char **end) {
    const char *s = skip_ws(nptr);
    int sign = 1;
    int any = 0;
    double acc = 0;
    if (*s == '+' || *s == '-') {
        sign = *s == '-' ? -1 : 1;
        s++;
    }
    while (isdigit((unsigned char)*s)) {
        acc = acc * 10.0 + (*s - '0');
        s++;
        any = 1;
    }
    if (*s == '.') {
        double place = 0.1;
        s++;
        while (isdigit((unsigned char)*s)) {
            acc += (*s - '0') * place;
            place *= 0.1;
            s++;
            any = 1;
        }
    }
    if (*s == 'e' || *s == 'E') {
        int esign = 1;
        int exp = 0;
        double mag = 1;
        s++;
        if (*s == '+' || *s == '-') {
            esign = *s == '-' ? -1 : 1;
            s++;
        }
        while (isdigit((unsigned char)*s)) {
            exp = exp * 10 + (*s - '0');
            s++;
            any = 1;
        }
        exp *= esign;
        while (exp > 0) {
            mag *= 10.0;
            exp--;
        }
        while (exp < 0) {
            mag /= 10.0;
            exp++;
        }
        acc *= mag;
    }
    if (end) {
        *end = (char *)(any ? s : nptr);
    }
    return sign < 0 ? -acc : acc;
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

double ldexp(double x, int exp) {
    if (exp > 0) {
        while (exp--) {
            x *= 2.0;
        }
    } else if (exp < 0) {
        while (exp++) {
            x *= 0.5;
        }
    }
    return x;
}

long double ldexpl(long double x, int exp) {
    if (exp > 0) {
        while (exp--) {
            x *= 2.0L;
        }
    } else if (exp < 0) {
        while (exp++) {
            x *= 0.5L;
        }
    }
    return x;
}

/* ---------------------------------------------------------------- qsort */

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *b = base;
    unsigned char *tmp;
    size_t start;
    size_t end;
    if (!b || n < 2 || size == 0 || !cmp) {
        return;
    }
    tmp = malloc(size);
    if (!tmp) {
        return;
    }
    start = n / 2;
    while (start > 0) {
        size_t root;
        start--;
        root = start;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= n) {
                break;
            }
            if (child + 1 < n && cmp(b + child * size, b + (child + 1) * size) < 0) {
                child++;
            }
            if (cmp(b + root * size, b + child * size) < 0) {
                memcpy(tmp, b + root * size, size);
                memcpy(b + root * size, b + child * size, size);
                memcpy(b + child * size, tmp, size);
                root = child;
            } else {
                break;
            }
        }
    }
    end = n;
    while (end > 1) {
        size_t root = 0;
        size_t count;
        end--;
        memcpy(tmp, b, size);
        memcpy(b, b + end * size, size);
        memcpy(b + end * size, tmp, size);
        count = end;
        for (;;) {
            size_t child = root * 2 + 1;
            if (child >= count) {
                break;
            }
            if (child + 1 < count && cmp(b + child * size, b + (child + 1) * size) < 0) {
                child++;
            }
            if (cmp(b + root * size, b + child * size) < 0) {
                memcpy(tmp, b + root * size, size);
                memcpy(b + root * size, b + child * size, size);
                memcpy(b + child * size, tmp, size);
                root = child;
            } else {
                break;
            }
        }
    }
    free(tmp);
}

/* ---------------------------------------------------------------- time */

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static const int mdays_n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const int mdays_l[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

time_t time(time_t *out) {
    fos_rtc_t r;
    time_t t = 0;
    int y;
    int m;
    const int *md;
    if (fosapi()->rtc_read(&r) != 0) {
        if (out) {
            *out = 0;
        }
        return 0;
    }
    for (y = 1970; y < (int)r.year; y++) {
        t += is_leap(y) ? 366 : 365;
    }
    md = is_leap(r.year) ? mdays_l : mdays_n;
    for (m = 1; m < (int)r.month && m <= 12; m++) {
        t += md[m - 1];
    }
    if (r.day) {
        t += r.day - 1;
    }
    t = t * 86400 + (time_t)r.hour * 3600 + (time_t)r.minute * 60 + r.second;
    if (out) {
        *out = t;
    }
    return t;
}

struct tm *localtime(const time_t *tp) {
    static struct tm tm;
    time_t t = tp ? *tp : 0;
    time_t days;
    int sec;
    int y = 1970;
    int m = 0;
    const int *md;
    memset(&tm, 0, sizeof tm);
    if (t < 0) {
        t = 0;
    }
    days = t / 86400;
    sec = (int)(t % 86400);
    tm.tm_sec = sec % 60;
    tm.tm_min = (sec / 60) % 60;
    tm.tm_hour = sec / 3600;
    for (;;) {
        int diy = is_leap(y) ? 366 : 365;
        if (days < diy || y > 2099) {
            break;
        }
        days -= diy;
        y++;
    }
    tm.tm_year = y - 1900;
    tm.tm_yday = (int)days;
    md = is_leap(y) ? mdays_l : mdays_n;
    while (m < 11 && days >= md[m]) {
        days -= md[m];
        m++;
    }
    tm.tm_mon = m;
    tm.tm_mday = (int)days + 1;
    return &tm;
}

int gettimeofday(struct timeval *tv, void *tz) {
    uint64_t ms;
    (void)tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    ms = fosapi()->get_ticks_ms();
    tv->tv_sec = (long)(ms / 1000);
    tv->tv_usec = (long)((ms % 1000) * 1000);
    return 0;
}

/* ---------------------------------------------------------------- misc */

char *getenv(const char *name) {
    if (!name) {
        return NULL;
    }
    return (char *)fosapi()->getenv(name);
}

static void path_collapse(char *s) {
    char *r = s;
    char *w = s;
    if (s[0] == '\\') {
        *w++ = '\\';
        r = s + 1;
    }
    while (*r) {
        char *part = r;
        size_t n = 0;
        while (r[n] && r[n] != '\\') {
            n++;
        }
        if (n == 1 && part[0] == '.') {
            /* skip */
        } else if (n == 2 && part[0] == '.' && part[1] == '.') {
            if (w > s + 1) {
                w--;
                while (w > s && w[-1] != '\\') {
                    w--;
                }
            }
        } else {
            size_t i;
            if (w > s && w[-1] != '\\') {
                *w++ = '\\';
            }
            for (i = 0; i < n; i++) {
                *w++ = part[i];
            }
        }
        r += n;
        if (*r == '\\') {
            r++;
        }
    }
    if (w == s) {
        *w++ = '\\';
    }
    *w = 0;
}

char *realpath(const char *path, char *resolved) {
    char tmp[260];
    char joined[260];
    size_t i;
    size_t o = 0;
    if (!path || !path[0]) {
        errno = EINVAL;
        return NULL;
    }
    for (i = 0; path[i] && o + 1 < sizeof tmp; i++) {
        tmp[o++] = (path[i] == '/') ? '\\' : path[i];
    }
    tmp[o] = 0;
    if (tmp[0] != '\\' && !(tmp[0] && tmp[1] == ':')) {
        char cwd[260];
        size_t cl;
        fosapi()->get_cwd(cwd, sizeof cwd);
        cl = strlen(cwd);
        if (cl + 1 + strlen(tmp) + 1 >= sizeof joined) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        memcpy(joined, cwd, cl);
        if (cl == 0 || joined[cl - 1] != '\\') {
            joined[cl++] = '\\';
        }
        strcpy(joined + cl, tmp);
    } else {
        strncpy(joined, tmp, sizeof joined - 1);
        joined[sizeof joined - 1] = 0;
    }
    path_collapse(joined);
    if (!resolved) {
        resolved = malloc(strlen(joined) + 1);
        if (!resolved) {
            errno = ENOMEM;
            return NULL;
        }
    }
    strcpy(resolved, joined);
    return resolved;
}

void exit(int status) {
    (void)status;
    if (fos_exit_ready) {
        longjmp(fos_exit_buf, 1);
    }
    for (;;) {
    }
}

void abort(void) {
    fosapi()->write_line("abort");
    exit(1);
}

int fos_make_argv(const char *argv0, char **argv, int max) {
    static char buf[256];
    const char *s = fosapi()->cmdline;
    char *d = buf;
    int argc = 0;
    if (max <= 0) {
        return 0;
    }
    argv[argc++] = (char *)(argv0 ? argv0 : "");
    if (!s) {
        return argc;
    }
    while (*s && argc < max) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (!*s) {
            break;
        }
        argv[argc++] = d;
        if (*s == '"') {
            s++;
            while (*s && *s != '"' && (size_t)(d - buf) + 1 < sizeof buf) {
                *d++ = *s++;
            }
            if (*s == '"') {
                s++;
            }
        } else {
            while (*s && *s != ' ' && *s != '\t' && (size_t)(d - buf) + 1 < sizeof buf) {
                *d++ = *s++;
            }
        }
        *d++ = 0;
    }
    return argc;
}
