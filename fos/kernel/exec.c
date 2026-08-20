/*
 * exec.c — FOSCOM (.COM) program loader.
 *
 * .COM files have a 64-byte FOSCOM header followed by flat x86-64 code.
 * The kernel streams the payload to load_addr, zeroes BSS, sets RSP, and
 * jumps to entry. Programs call the fixed API block at FOS_API_ADDR.
 *
 * Nested run_com (FM→EDIT) reuses the same load address, so the outer
 * image is snapshotted to the heap first and restored after the inner
 * program returns. Cmdline/pipe are saved separately (fos_api_save_io).
 */

#include "foscom.h"
#include "fos_api.h"
#include "vfs.h"
#include "string.h"
#include "irq.h"
#include "heap.h"
#include "memory.h"
#include "mouse.h"

extern void com_call(void (*entry)(void), uint64_t stack_top);

static int com_level;

typedef struct {
    uint64_t load_addr;
    uint64_t mem_span;
    void    *saved;
} com_image_t;

static com_image_t com_loaded[COM_STACK_LEVELS];

static int ranges_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return an && bn && a < b + bn && b < a + an;
}

static void discard_saved(com_image_t *img) {
    if (img && img->saved) {
        heap_free(img->saved);
        img->saved = 0;
    }
}

static int restore_outer(void) {
    com_image_t *outer;

    if (com_level <= 0) {
        return 0;
    }
    outer = &com_loaded[com_level - 1];
    if (!outer->saved) {
        return 0;
    }
    memcpy((void *)(uintptr_t)outer->load_addr, outer->saved, (size_t)outer->mem_span);
    discard_saved(outer);
    return 0;
}

static int save_outer(void) {
    com_image_t *outer;

    if (com_level <= 0) {
        return 0;
    }
    outer = &com_loaded[com_level - 1];
    discard_saved(outer);
    if (outer->mem_span == 0) {
        return 0;
    }
    outer->saved = heap_alloc((size_t)outer->mem_span);
    if (!outer->saved) {
        return -1;
    }
    memcpy(outer->saved, (void *)(uintptr_t)outer->load_addr, (size_t)outer->mem_span);
    return 0;
}

static int load_payload(int drive, const char *path, const foscom_hdr_t *hdr,
                        uint64_t mem_span) {
    uint64_t left = hdr->payload_size;
    uint32_t off = (uint32_t)sizeof(foscom_hdr_t);
    uint8_t *dest = (uint8_t *)(uintptr_t)hdr->load_addr;

    while (left) {
        uint32_t chunk = left > 0x10000ull ? 0x10000u : (uint32_t)left;
        uint32_t got = 0;

        if (vfs_read_at(drive, path, off, dest, chunk, &got) != 0 || got == 0) {
            return -1;
        }
        dest += got;
        off += got;
        left -= got;
    }
    if (mem_span > hdr->payload_size) {
        memset((uint8_t *)(uintptr_t)hdr->load_addr + hdr->payload_size, 0,
               (size_t)(mem_span - hdr->payload_size));
    }
    return 0;
}

static int load_and_run(int drive, const char *path, const foscom_hdr_t *hdr) {
    uint64_t mem_span;
    uint64_t stack;
    uint16_t owner;
    uint16_t caller_owner;
    int rc = -1;

    if (hdr->load_addr < COM_LOAD_MIN || hdr->entry < hdr->load_addr) {
        return -1;
    }
    if (hdr->load_addr >= COM_LOAD_MAX) {
        return -1;
    }
    if (com_level >= COM_STACK_LEVELS) {
        return -1;
    }
    if (hdr->payload_size == 0 || hdr->mem_size < hdr->payload_size) {
        return -1;
    }
    /* vfs_read_at offsets are 32-bit. */
    if (hdr->payload_size > 0xFFFFFFFFu - sizeof(foscom_hdr_t)) {
        return -1;
    }

    /* Normalise mem_size: it may be an end address or a span. */
    mem_span = hdr->mem_size;
    if (mem_span > hdr->load_addr && mem_span < hdr->load_addr + 0x10000000ULL) {
        mem_span -= hdr->load_addr;
    }
    if (mem_span > COM_LOAD_MAX - hdr->load_addr) {
        return -1; /* image would reach into the program stacks */
    }
    if (hdr->entry >= hdr->load_addr + mem_span) {
        return -1;
    }
    /* The API block sits inside the historical COM window (0xFF0000).
     * Loading or BSS-zeroing through it would NULL every COM function pointer. */
    if (ranges_overlap(hdr->load_addr, mem_span, FOS_API_ADDR, sizeof(fos_api_t))) {
        return -1;
    }

    /* Snapshot is kernel-owned so heap_free_owner() of the inner program
     * cannot throw it away. */
    caller_owner = heap_set_owner(0);
    if (save_outer() != 0) {
        heap_set_owner(caller_owner);
        return -1;
    }

    if (load_payload(drive, path, hdr, mem_span) != 0) {
        restore_outer();
        heap_set_owner(caller_owner);
        return -1;
    }

    com_loaded[com_level].load_addr = hdr->load_addr;
    com_loaded[com_level].mem_span = mem_span;
    com_loaded[com_level].saved = 0;

    /* Honour a requested stack only inside the safe window; images packed
     * before the stacks moved still carry an old value and get the kernel slot. */
    stack = COM_STACK_TOP - (uint64_t)com_level * COM_STACK_SIZE;
    if (com_level == 0 && hdr->stack_top > COM_LOAD_MAX &&
        hdr->stack_top <= COM_STACK_TOP) {
        stack = hdr->stack_top;
    }

    /* Tag the program's allocations so they can be reclaimed even if it
     * exits without freeing (or crashes out of a nested run_com). */
    owner = (uint16_t)(com_level + 1);
    heap_set_owner(owner);

    com_level++;
    com_call((void (*)(void))(uintptr_t)hdr->entry, stack);
    com_level--;

    heap_set_owner(0);
    heap_free_owner(owner);
    restore_outer();
    /* Drop COM-registered PIC handlers. Nested run_com (FM→PLAY) overwrites
     * the load address, so a leftover handler would jump into the restored
     * outer image. Kernel timer IRQ0 is dispatched separately. */
    irq_clear_handlers();
    mouse_irq_restore();
    heap_set_owner(caller_owner);
    rc = 0;
    return rc;
}

int exec_run(int drive, const char *path, const char *cmdline) {
    uint8_t raw[sizeof(foscom_hdr_t)];
    uint32_t got = 0;
    foscom_hdr_t hdr;
    static fos_api_io_t nest_io[COM_STACK_LEVELS];
    int slot;
    int rc;

    if (vfs_read_at(drive, path, 0, raw, (uint32_t)sizeof(raw), &got) != 0 ||
        got < sizeof(hdr)) {
        return -1;
    }
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.magic != FOSCOM_MAGIC || hdr.version != FOSCOM_VERSION) {
        return -1;
    }

    slot = com_level;
    if (slot < 0 || slot >= COM_STACK_LEVELS) {
        return -1;
    }
    fos_api_save_io(&nest_io[slot]);
    fos_api_set_cmdline(cmdline ? cmdline : "");
    rc = load_and_run(drive, path, &hdr);
    fos_api_restore_io(&nest_io[slot]);
    return rc;
}
