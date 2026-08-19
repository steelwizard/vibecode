#pragma once

#include "types.h"

/* Fixed kernel API block — .COM programs call these instead of linking drivers. */

#define FOS_API_ADDR   0x0000000000FF0000ULL
#define FOS_API_MAGIC  0x49534F46u /* 'FOSI' */

typedef struct {
    uint32_t magic;
    void (*write)(const char *s);
    void (*write_n)(const char *s, size_t n);
    void (*write_line)(const char *s);
    void (*putchar)(char c);
    char     cmdline[256];
    char     pipe_in[4096];
    size_t   pipe_in_len;
} fos_api_t;

void fos_api_init(void);
void fos_api_set_cmdline(const char *args);
void fos_api_set_pipe(const char *data, size_t len);
void fos_api_clear_pipe(void);
