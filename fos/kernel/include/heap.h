#pragma once

#include "types.h"

/*
 * Byte-granular heap over the physical page pool (memory.c).
 *
 * Blocks are tagged with an owner so a .COM that exits without freeing does
 * not leak: exec.c reclaims everything the program allocated. Owner 0 is the
 * kernel. Not interrupt-safe — never allocate from an IRQ handler.
 */

void  heap_init(void);
void *heap_alloc(size_t bytes);
void  heap_free(void *ptr);
void *heap_realloc(void *ptr, size_t bytes);

/* Blocks allocated from now on belong to `owner`; returns the previous owner. */
uint16_t heap_set_owner(uint16_t owner);
uint16_t heap_get_owner(void);
void     heap_free_owner(uint16_t owner);

void heap_stats(uint64_t *reserved_bytes, uint64_t *used_bytes, uint64_t *blocks);
