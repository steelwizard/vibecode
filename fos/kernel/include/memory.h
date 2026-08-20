#pragma once

#include "fos_api.h"

#define MEM_PAGE_SIZE 4096ULL

void memory_init(void);
void memory_set_kernel_size(uint64_t size);
int  memory_get_info(fos_mem_info_t *out);

/* Physical page pool. Not interrupt-safe: call from normal kernel/.COM
 * context, never from an IRQ handler. */
void  memory_pages_init(void);
void *memory_alloc_pages(size_t pages);
void  memory_free_pages(void *addr, size_t pages);
void  memory_pages_stats(uint64_t *total_bytes, uint64_t *used_bytes);
