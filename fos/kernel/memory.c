/*
 * memory.c — Physical RAM map from BIOS E820 (collected in boot stage2).
 */

#include "memory.h"
#include "string.h"

#define MEM_MAP_ADDR   0x4000ULL
#define MEM_MAP_MAGIC  0x50414D4Du /* 'MMAP' */

#define E820_USABLE    1u
#define E820_RESERVED  2u
#define E820_ACPI      3u
#define E820_NVS       4u
#define E820_BAD       5u

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} e820_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    e820_entry_t entries[FOS_MEM_REGION_MAX];
} e820_map_t;

/*
 * Page pool: [POOL_BASE, POOL_LIMIT), minus whatever E820 says is not RAM.
 *
 * Everything the system pins lives below 16 MiB — low memory and the SB16 DMA
 * buffer, the kernel image and its BSS, .COM images, the program stacks, and
 * the API block at 0xFF0000 — so a pool starting at 16 MiB needs no per-region
 * carve-outs. The top is the identity map limit: the bootloader maps 32 × 2 MiB
 * and nothing above that is addressable yet.
 */
#define POOL_BASE      0x1000000ULL
#define POOL_LIMIT     0x4000000ULL
#define POOL_MAX_PAGES ((POOL_LIMIT - POOL_BASE) / MEM_PAGE_SIZE)

static const e820_map_t *map;
static uint64_t kernel_image_size;

static uint8_t  page_used[POOL_MAX_PAGES / 8]; /* 1 = allocated or not RAM */
static uint64_t pool_pages;                    /* pages backed by usable RAM */
static uint64_t pool_used;

static int map_valid(void) {
    return map && map->magic == MEM_MAP_MAGIC && map->count <= FOS_MEM_REGION_MAX;
}

static int bit_get(uint64_t i) {
    return (page_used[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint64_t i, int value) {
    uint8_t mask = (uint8_t)(1u << (i & 7));
    if (value) {
        page_used[i >> 3] |= mask;
    } else {
        page_used[i >> 3] &= (uint8_t)~mask;
    }
}

static int page_is_ram(uint64_t addr) {
    for (uint32_t i = 0; i < map->count; i++) {
        const e820_entry_t *e = &map->entries[i];
        if (e->type != E820_USABLE) {
            continue;
        }
        if (addr >= e->base && addr + MEM_PAGE_SIZE <= e->base + e->length) {
            return 1;
        }
    }
    return 0;
}

void memory_pages_init(void) {
    uint64_t i;

    memset(page_used, 0xFF, sizeof(page_used));
    pool_pages = 0;
    pool_used = 0;

    if (!map_valid()) {
        return; /* no map, no pool — allocations just fail */
    }

    for (i = 0; i < POOL_MAX_PAGES; i++) {
        if (page_is_ram(POOL_BASE + i * MEM_PAGE_SIZE)) {
            bit_set(i, 0);
            pool_pages++;
        }
    }
}

void *memory_alloc_pages(size_t pages) {
    uint64_t need = pages;
    uint64_t i = 0;

    if (need == 0 || need > POOL_MAX_PAGES) {
        return 0;
    }

    while (i + need <= POOL_MAX_PAGES) {
        uint64_t j = 0;
        while (j < need && !bit_get(i + j)) {
            j++;
        }
        if (j == need) {
            for (j = 0; j < need; j++) {
                bit_set(i + j, 1);
            }
            pool_used += need;
            return (void *)(uintptr_t)(POOL_BASE + i * MEM_PAGE_SIZE);
        }
        i += j + 1; /* the blocker is at i + j, so resume past it */
    }
    return 0;
}

void memory_free_pages(void *addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t first, i;

    if (!addr || pages == 0 || (base & (MEM_PAGE_SIZE - 1)) != 0) {
        return;
    }
    if (base < POOL_BASE || base + pages * MEM_PAGE_SIZE > POOL_LIMIT) {
        return;
    }

    first = (base - POOL_BASE) / MEM_PAGE_SIZE;
    for (i = 0; i < pages; i++) {
        if (bit_get(first + i)) {
            bit_set(first + i, 0);
            pool_used--;
        }
    }
}

void memory_pages_stats(uint64_t *total_bytes, uint64_t *used_bytes) {
    if (total_bytes) {
        *total_bytes = pool_pages * MEM_PAGE_SIZE;
    }
    if (used_bytes) {
        *used_bytes = pool_used * MEM_PAGE_SIZE;
    }
}

void memory_init(void) {
    map = (const e820_map_t *)(uintptr_t)MEM_MAP_ADDR;
    if (!map_valid()) {
        map = 0;
    }
}

void memory_set_kernel_size(uint64_t size) {
    kernel_image_size = size;
}

int memory_get_info(fos_mem_info_t *out) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (!map_valid()) {
        return -1;
    }

    out->kernel_base = 0x100000ULL;
    out->kernel_size = kernel_image_size;

    for (uint32_t i = 0; i < map->count; i++) {
        const e820_entry_t *e = &map->entries[i];
        if (out->count >= FOS_MEM_REGION_MAX) {
            break;
        }

        out->regions[out->count].base = e->base;
        out->regions[out->count].length = e->length;
        out->regions[out->count].type = e->type;
        out->count++;

        out->total_bytes += e->length;
        if (e->type == E820_USABLE) {
            out->usable_bytes += e->length;
        } else {
            out->reserved_bytes += e->length;
        }
    }

    return 0;
}
