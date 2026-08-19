#include "string.h"

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++)) {
    }
    return out;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = 0;
    }
    return dst;
}

void *memset(void *dst, int value, size_t count) {
    uint8_t *p = (uint8_t *)dst;
    while (count--) {
        *p++ = (uint8_t)value;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (count--) {
        *d++ = *s++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) {
        if (*p != *q) {
            return *p - *q;
        }
        p++;
        q++;
    }
    return 0;
}

static int lower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + 32;
    }
    return c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower(*a) - lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n > 0 && *a && lower(*a) == lower(*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return lower(*a) - lower(*b);
}

const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

int is_digit(char c) {
    return c >= '0' && c <= '9';
}

int atoi(const char *s) {
    int n = 0;
    s = skip_spaces(s);
    while (is_digit(*s)) {
        n = n * 10 + (*s - '0');
        s++;
    }
    return n;
}

/* Copy src into dst, normalizing '/' to '\\' */
void path_normalize_slashes(char *dst, const char *src, size_t sz) {
    size_t i = 0;
    while (src[i] && i + 1 < sz) {
        dst[i] = (src[i] == '/') ? '\\' : src[i];
        i++;
    }
    dst[i] = 0;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == 0) ? (char *)s : 0;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *out = dst;
    while (*dst) {
        dst++;
    }
    while (n > 0 && *src) {
        *dst++ = *src++;
        n--;
    }
    *dst = 0;
    return out;
}

void *memmove(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || count == 0) {
        return dst;
    }
    if (d < s) {
        while (count--) {
            *d++ = *s++;
        }
    } else {
        d += count;
        s += count;
        while (count--) {
            *--d = *--s;
        }
    }
    return dst;
}
