/*
 * exec.c — FOSCOM (.COM) program loader.
 *
 * .COM files have a 64-byte FOSCOM header followed by flat x86-64 code.
 * The kernel copies the payload to load_addr, zeroes BSS, sets RSP, and
 * jumps to entry. Programs call the fixed API block at 0xFF0000 (fos_api).
 */

#include "foscom.h"
#include "fos_api.h"
#include "vfs.h"
#include "console.h"
#include "string.h"

#define EXEC_BUF_MAX 65536
#define DEFAULT_STACK 0x8F000ULL

static int load_and_run(const foscom_hdr_t *hdr, const void *payload) {
    if (hdr->load_addr < 0x100000 || hdr->entry < hdr->load_addr) {
        return -1;
    }
    if (hdr->payload_size == 0 || hdr->mem_size < hdr->payload_size) {
        return -1;
    }
    if (hdr->payload_size > EXEC_BUF_MAX - sizeof(foscom_hdr_t)) {
        return -1;
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)hdr->load_addr;
    memcpy(dest, payload, (size_t)hdr->payload_size);

    /* Zero the mem_size region beyond the loaded payload (BSS). */
    uint64_t mem_span = hdr->mem_size;
    if (mem_span > hdr->load_addr && mem_span < hdr->load_addr + 0x10000000ULL) {
        mem_span -= hdr->load_addr;
    }
    if (mem_span > hdr->payload_size) {
        memset(dest + hdr->payload_size, 0, (size_t)(mem_span - hdr->payload_size));
    }

    uint64_t stack = hdr->stack_top ? hdr->stack_top : DEFAULT_STACK;
    void (*entry)(void) = (void (*)(void))(uintptr_t)hdr->entry;
    uint64_t saved_rsp;

    /* Run .COM on its own stack; restore kernel RSP when it returns. */
    __asm__ volatile("mov %%rsp, %0" : "=r"(saved_rsp));
    __asm__ volatile("mov %0, %%rsp" : : "r"(stack) : "memory");
    entry();
    __asm__ volatile("mov %0, %%rsp" : : "r"(saved_rsp) : "memory");
    return 0;
}

int exec_run(int drive, const char *path, const char *cmdline) {
    static uint8_t filebuf[EXEC_BUF_MAX];
    size_t len = 0;

    if (vfs_read_file(drive, path, (char *)filebuf, sizeof(filebuf), &len) != 0) {
        return -1;
    }
    if (len < sizeof(foscom_hdr_t)) {
        return -1;
    }

    foscom_hdr_t *hdr = (foscom_hdr_t *)filebuf;
    if (hdr->magic != FOSCOM_MAGIC || hdr->version != FOSCOM_VERSION) {
        return -1;
    }
    if (sizeof(foscom_hdr_t) + hdr->payload_size > len) {
        return -1;
    }

    fos_api_set_cmdline(cmdline ? cmdline : "");
    return load_and_run(hdr, filebuf + sizeof(foscom_hdr_t));
}
