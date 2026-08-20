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
#include "irq.h"
#include "heap.h"

#define EXEC_BUF_MAX 131072

/*
 * Program stacks live well above the kernel's, which grows down from 0x90000.
 * The kernel is already ~12 KB deep by the time it starts a program, so a
 * stack placed just below 0x90000 would sit inside live kernel frames and a
 * deep program (an MP3 decoder, say) would quietly shred them. Each nesting
 * level of api->run_com gets its own slot below COM_STACK_TOP.
 */
#define COM_STACK_TOP    0x800000ULL
#define COM_STACK_SIZE   0x100000ULL
#define COM_STACK_LEVELS 4
#define COM_LOAD_MIN     0x200000ULL
#define COM_LOAD_MAX     (COM_STACK_TOP - COM_STACK_LEVELS * COM_STACK_SIZE)

extern void com_call(void (*entry)(void), uint64_t stack_top);

static int com_level;

static int load_and_run(const foscom_hdr_t *hdr, const void *payload) {
    if (hdr->load_addr < COM_LOAD_MIN || hdr->entry < hdr->load_addr) {
        return -1;
    }
    if (com_level >= COM_STACK_LEVELS) {
        return -1;
    }
    if (hdr->payload_size == 0 || hdr->mem_size < hdr->payload_size) {
        return -1;
    }
    if (hdr->payload_size > EXEC_BUF_MAX - sizeof(foscom_hdr_t)) {
        return -1;
    }

    /* Normalise mem_size: it may be an end address or a span. */
    uint64_t mem_span = hdr->mem_size;
    if (mem_span > hdr->load_addr && mem_span < hdr->load_addr + 0x10000000ULL) {
        mem_span -= hdr->load_addr;
    }
    if (hdr->load_addr + mem_span > COM_LOAD_MAX) {
        return -1; /* image would reach into the program stacks */
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)hdr->load_addr;
    memcpy(dest, payload, (size_t)hdr->payload_size);
    if (mem_span > hdr->payload_size) {
        memset(dest + hdr->payload_size, 0, (size_t)(mem_span - hdr->payload_size));
    }

    /* Honour a requested stack only inside the safe window; images packed
     * before the stacks moved still carry the old kernel-adjacent value. */
    uint64_t stack = COM_STACK_TOP - (uint64_t)com_level * COM_STACK_SIZE;
    if (com_level == 0 && hdr->stack_top > COM_LOAD_MAX &&
        hdr->stack_top <= COM_STACK_TOP) {
        stack = hdr->stack_top;
    }

    /* Tag the program's allocations so they can be reclaimed even if it
     * exits without freeing (or crashes out of a nested run_com). */
    uint16_t owner = (uint16_t)(com_level + 1);
    uint16_t prev_owner = heap_set_owner(owner);

    com_level++;
    com_call((void (*)(void))(uintptr_t)hdr->entry, stack);
    com_level--;

    heap_set_owner(prev_owner);
    heap_free_owner(owner);
    irq_clear_handlers();
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
