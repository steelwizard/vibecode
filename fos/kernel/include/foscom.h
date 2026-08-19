#pragma once

#include "types.h"

/* FOS .COM executable header (64 bytes, DOS-style naming). */

#define FOSCOM_MAGIC   0x4F435346u /* 'FSCO' little-endian */
#define FOSCOM_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t entry;
    uint64_t load_addr;
    uint64_t payload_size; /* bytes after this header */
    uint64_t mem_size;     /* total image size at load_addr (includes BSS) */
    uint64_t stack_top;    /* RSP before entry; 0 = leave unchanged */
    char     name[16];
} foscom_hdr_t;

int exec_run(int drive, const char *path, const char *cmdline);
