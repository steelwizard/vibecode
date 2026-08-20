/*
 * heap.c — first-fit heap over the physical page pool.
 *
 * One address-ordered list holds every block from every page run. Adjacent
 * free blocks coalesce, and a run whose pages are entirely free is handed back
 * to the page allocator, so a program that briefly needs a lot of memory does
 * not keep it reserved for the rest of the session.
 */

#include "heap.h"
#include "memory.h"
#include "string.h"

#define ALIGN     16ULL
#define MIN_ALLOC 16ULL
#define GROW_MIN  16ULL /* pages: grow in 64 KiB steps to limit fragmentation */

typedef struct heap_block {
    struct heap_block *next;
    struct heap_block *prev;
    uint64_t size;      /* payload bytes */
    uint32_t run_pages; /* nonzero => block starts a page run of this many pages */
    uint16_t used;
    uint16_t owner;
} heap_block_t;

static heap_block_t *head;
static uint16_t current_owner;

static uint64_t align_up(uint64_t v) {
    return (v + (ALIGN - 1)) & ~(ALIGN - 1);
}

static void *payload_of(heap_block_t *b) {
    return (void *)(b + 1);
}

/* True when `b` is immediately followed in memory by `b->next`, and that
 * neighbour is not the head of its own run (runs must stay separable). */
static int adjacent(heap_block_t *b) {
    if (!b || !b->next || b->next->run_pages) {
        return 0;
    }
    return (uint8_t *)b + sizeof(heap_block_t) + b->size == (uint8_t *)b->next;
}

static void unlink_block(heap_block_t *b) {
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        head = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
}

static void insert_ordered(heap_block_t *b) {
    heap_block_t *p = 0;
    heap_block_t *c = head;

    while (c && c < b) {
        p = c;
        c = c->next;
    }
    b->prev = p;
    b->next = c;
    if (p) {
        p->next = b;
    } else {
        head = b;
    }
    if (c) {
        c->prev = b;
    }
}

void heap_init(void) {
    head = 0;
    current_owner = 0;
}

static int grow(uint64_t need) {
    uint64_t pages = (need + sizeof(heap_block_t) + MEM_PAGE_SIZE - 1) / MEM_PAGE_SIZE;
    heap_block_t *b;
    void *mem;

    if (pages < GROW_MIN) {
        pages = GROW_MIN;
    }
    mem = memory_alloc_pages((size_t)pages);
    if (!mem && pages > GROW_MIN) {
        /* Retry at the exact size in case the pool is fragmented. */
        pages = (need + sizeof(heap_block_t) + MEM_PAGE_SIZE - 1) / MEM_PAGE_SIZE;
        mem = memory_alloc_pages((size_t)pages);
    }
    if (!mem) {
        return -1;
    }

    b = (heap_block_t *)mem;
    b->size = pages * MEM_PAGE_SIZE - sizeof(heap_block_t);
    b->run_pages = (uint32_t)pages;
    b->used = 0;
    b->owner = 0;
    insert_ordered(b);
    return 0;
}

static heap_block_t *find_fit(uint64_t need) {
    for (heap_block_t *b = head; b; b = b->next) {
        if (!b->used && b->size >= need) {
            return b;
        }
    }
    return 0;
}

void *heap_alloc(size_t bytes) {
    uint64_t need = align_up(bytes < MIN_ALLOC ? MIN_ALLOC : bytes);
    heap_block_t *b;

    if (bytes == 0 || need < bytes) {
        return 0; /* zero, or an overflowing size */
    }

    b = find_fit(need);
    if (!b) {
        if (grow(need) != 0) {
            return 0;
        }
        b = find_fit(need);
        if (!b) {
            return 0;
        }
    }

    /* Split when the tail can hold a header plus a usable payload. */
    if (b->size >= need + sizeof(heap_block_t) + MIN_ALLOC) {
        heap_block_t *tail = (heap_block_t *)((uint8_t *)b + sizeof(heap_block_t) + need);
        tail->size = b->size - need - sizeof(heap_block_t);
        tail->run_pages = 0;
        tail->used = 0;
        tail->owner = 0;
        tail->prev = b;
        tail->next = b->next;
        if (b->next) {
            b->next->prev = tail;
        }
        b->next = tail;
        b->size = need;
    }

    b->used = 1;
    b->owner = current_owner;
    return payload_of(b);
}

/* Only trust a pointer we actually handed out: a wild or double free would
 * otherwise corrupt the list, and .COM code runs at the same privilege. */
static heap_block_t *find_block(void *ptr) {
    heap_block_t *target = (heap_block_t *)ptr - 1;

    for (heap_block_t *b = head; b; b = b->next) {
        if (b == target) {
            return b->used ? b : 0;
        }
    }
    return 0;
}

void heap_free(void *ptr) {
    heap_block_t *b;

    if (!ptr) {
        return;
    }
    b = find_block(ptr);
    if (!b) {
        return;
    }

    b->used = 0;
    b->owner = 0;

    if (adjacent(b) && !b->next->used) {
        heap_block_t *n = b->next;
        b->size += sizeof(heap_block_t) + n->size;
        unlink_block(n);
    }
    if (b->prev && !b->prev->used && adjacent(b->prev)) {
        heap_block_t *p = b->prev;
        p->size += sizeof(heap_block_t) + b->size;
        unlink_block(b);
        b = p;
    }

    /* Whole run free again? Give the pages back. */
    if (b->run_pages && sizeof(heap_block_t) + b->size == (uint64_t)b->run_pages * MEM_PAGE_SIZE) {
        uint32_t pages = b->run_pages;
        unlink_block(b);
        memory_free_pages(b, pages);
    }
}

void *heap_realloc(void *ptr, size_t bytes) {
    heap_block_t *b;
    void *fresh;

    if (!ptr) {
        return heap_alloc(bytes);
    }
    if (bytes == 0) {
        heap_free(ptr);
        return 0;
    }

    b = find_block(ptr);
    if (!b) {
        return 0;
    }
    if (b->size >= align_up(bytes)) {
        return ptr; /* already big enough */
    }
    fresh = heap_alloc(bytes);
    if (!fresh) {
        return 0;
    }
    memcpy(fresh, ptr, b->size < bytes ? (size_t)b->size : bytes);
    heap_free(ptr);
    return fresh;
}

uint16_t heap_set_owner(uint16_t owner) {
    uint16_t prev = current_owner;
    current_owner = owner;
    return prev;
}

void heap_free_owner(uint16_t owner) {
    /* Freeing coalesces and can release runs, so restart the walk each time
     * rather than holding a pointer across the mutation. */
    for (;;) {
        heap_block_t *b = head;
        while (b && !(b->used && b->owner == owner)) {
            b = b->next;
        }
        if (!b) {
            return;
        }
        heap_free(payload_of(b));
    }
}

void heap_stats(uint64_t *reserved_bytes, uint64_t *used_bytes, uint64_t *blocks) {
    uint64_t reserved = 0;
    uint64_t used = 0;
    uint64_t count = 0;

    for (heap_block_t *b = head; b; b = b->next) {
        if (b->run_pages) {
            reserved += (uint64_t)b->run_pages * MEM_PAGE_SIZE;
        }
        if (b->used) {
            used += b->size;
            count++;
        }
    }
    if (reserved_bytes) {
        *reserved_bytes = reserved;
    }
    if (used_bytes) {
        *used_bytes = used;
    }
    if (blocks) {
        *blocks = count;
    }
}
