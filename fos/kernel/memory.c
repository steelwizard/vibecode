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

static const e820_map_t *map;
static uint64_t kernel_image_size;

static int map_valid(void) {
    return map && map->magic == MEM_MAP_MAGIC && map->count <= FOS_MEM_REGION_MAX;
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
