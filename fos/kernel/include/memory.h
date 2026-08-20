#pragma once

#include "fos_api.h"

#define MEM_PAGE_SIZE 4096ULL

/*
 * Physical layout (identity-mapped).
 *
 * The boot PD has 512 × 2 MiB slots covering 0–1 GiB. The first
 * MEM_IDENTITY_PAGES of those are RAM; 0x20000000+ is left for the
 * framebuffer/MMIO windows in video.c.
 *
 *   0x00100000            kernel
 *   0x00FF0000            FOS API block (a hole inside the COM window —
 *                         exec.c refuses images that overlap it)
 *   COM_LOAD_MIN..MAX     .COM image (code + data + BSS)
 *   ..COM_STACK_TOP       8 MiB stacks, one slot per run_com nest
 *   COM_STACK_TOP..END    page pool / heap
 */
#define MEM_PD_ADDR          0x7000ULL
#define MEM_IDENTITY_PAGES   256ULL
#define MEM_IDENTITY_END     (MEM_IDENTITY_PAGES << 21) /* 512 MiB */

#define COM_LOAD_MIN         0x200000ULL
#define COM_LOAD_MAX         0x2300000ULL  /* 32 MiB at the usual 0x300000 */
#define COM_STACK_SIZE       0x800000ULL   /* 8 MiB per nesting level */
#define COM_STACK_LEVELS     4
#define COM_STACK_TOP        (COM_LOAD_MAX + COM_STACK_LEVELS * COM_STACK_SIZE)

#define MEM_POOL_BASE        COM_STACK_TOP
#define MEM_POOL_LIMIT       MEM_IDENTITY_END

void memory_init(void);
void memory_set_kernel_size(uint64_t size);
int  memory_get_info(fos_mem_info_t *out);

/* Physical page pool. Not interrupt-safe: call from normal kernel/.COM
 * context, never from an IRQ handler. */
void  memory_pages_init(void);
void *memory_alloc_pages(size_t pages);
void  memory_free_pages(void *addr, size_t pages);
void  memory_pages_stats(uint64_t *total_bytes, uint64_t *used_bytes);
