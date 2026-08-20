/*
 * mem.c — Physical RAM report (BIOS E820 map + kernel footprint).
 */

#include "fos_api.h"

static void write_str(fos_api_t *api, const char *s) {
    api->write(s);
}

static void write_line(fos_api_t *api, const char *s) {
    api->write_line(s);
}

static void write_dec(fos_api_t *api, uint64_t value) {
    char buf[24];
    int n = 0;
    char tmp[24];
    int t = 0;

    if (value == 0) {
        api->putchar('0');
        return;
    }

    while (value) {
        tmp[t++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (t--) {
        buf[n++] = tmp[t];
    }
    buf[n] = 0;
    api->write(buf);
}

static void write_hex64(fos_api_t *api, uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 15; i >= 0; i--) {
        buf[i] = hex[value & 0xFULL];
        value >>= 4;
    }
    buf[16] = 0;
    api->write(buf);
}

static void write_size(fos_api_t *api, uint64_t bytes) {
    static const char *units[] = {" B", " KB", " MB", " GB", " TB"};
    uint64_t whole = bytes;
    int unit = 0;

    while (whole >= 1024 && unit < 4) {
        whole /= 1024;
        unit++;
    }

    write_dec(api, whole);
    write_str(api, units[unit]);
}

static const char *type_name(uint32_t type) {
    switch (type) {
    case 1:
        return "Usable";
    case 2:
        return "Reserved";
    case 3:
        return "ACPI reclaim";
    case 4:
        return "ACPI NVS";
    case 5:
        return "Bad";
    default:
        return "Other";
    }
}

static void pad_field(fos_api_t *api, int width) {
    while (width-- > 0) {
        api->putchar(' ');
    }
}

static void write_padded(fos_api_t *api, const char *s, int width) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    api->write(s);
    pad_field(api, width - n);
}

static void print_region_table(fos_api_t *api, const fos_mem_info_t *info) {
    write_line(api, "Physical RAM (BIOS E820)");
    write_line(api, "  #   Base           Length         Type");
    write_line(api, "  --  -------------  -------------  ------------");

    for (int i = 0; i < info->count; i++) {
        const fos_mem_region_t *r = &info->regions[i];
        char idx[4];

        api->write("  ");
        if (i < 10) {
            api->putchar((char)('0' + i));
            api->write("   ");
        } else {
            idx[0] = (char)('0' + i / 10);
            idx[1] = (char)('0' + i % 10);
            idx[2] = 0;
            api->write(idx);
            api->putchar(' ');
        }
        write_hex64(api, r->base);
        api->write("  ");
        write_hex64(api, r->length);
        api->write("  ");
        write_padded(api, type_name(r->type), 12);
        api->putchar('\n');
    }
}

static void print_summary(fos_api_t *api, const fos_mem_info_t *info) {
    write_line(api, "");
    write_line(api, "Summary");
    api->write("  Total installed:  ");
    write_size(api, info->total_bytes);
    api->putchar('\n');
    api->write("  Usable RAM:       ");
    write_size(api, info->usable_bytes);
    api->putchar('\n');
    api->write("  Reserved/other:   ");
    write_size(api, info->reserved_bytes);
    api->putchar('\n');
}

static void print_kernel_map(fos_api_t *api, const fos_mem_info_t *info) {
    write_line(api, "");
    write_line(api, "Kernel and fixed addresses");
    api->write("  Kernel image:     0x");
    write_hex64(api, info->kernel_base);
    api->write(" (");
    write_size(api, info->kernel_size);
    api->write(")\n");
    write_line(api, "  Page tables:      0x5000 - 0x7FFF");
    write_line(api, "  E820 map stash:   0x4000");
    write_line(api, "  FOS API block:    0xFF0000");
    write_line(api, "  .COM load addr:   0x300000");
    write_line(api, "  .COM stack top:   0x800000");
    write_line(api, "  Kernel stack top: 0x90000");
}

static void print_largest_usable(fos_api_t *api, const fos_mem_info_t *info) {
    uint64_t best_base = 0;
    uint64_t best_len = 0;

    for (int i = 0; i < info->count; i++) {
        if (info->regions[i].type == 1 && info->regions[i].length > best_len) {
            best_len = info->regions[i].length;
            best_base = info->regions[i].base;
        }
    }

    write_line(api, "");
    api->write("  Largest usable region: ");
    if (best_len == 0) {
        write_line(api, "(none)");
        return;
    }
    write_size(api, best_len);
    api->write(" @ 0x");
    write_hex64(api, best_base);
    api->putchar('\n');
}

static void print_heap(fos_api_t *api) {
    fos_heap_info_t h;

    write_line(api, "");
    write_line(api, "Allocator");
    if (!api->get_heap_info || api->get_heap_info(&h) != 0) {
        write_line(api, "  (heap API missing — rebuild the kernel)");
        return;
    }
    api->write("  Page pool:        ");
    write_size(api, h.pool_total);
    api->write(" total, ");
    write_size(api, h.pool_used);
    api->write(" held by the heap\n");
    api->write("  Heap in use:      ");
    write_size(api, h.heap_used);
    api->write(" across ");
    write_dec(api, h.heap_blocks);
    api->write(" block(s)\n");
}

/* --- heap stress test (mem test) --- */

#define TEST_PTRS 256

static void *ptrs[TEST_PTRS];
static uint32_t sizes[TEST_PTRS];
static uint32_t rnd_state = 2463534242u;

static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void fill(void *p, uint32_t n, uint8_t seed) {
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        b[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static int check(const void *p, uint32_t n, uint8_t seed) {
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i] != (uint8_t)(seed + (uint8_t)i)) {
            return 0;
        }
    }
    return 1;
}

static void report(fos_api_t *api, const char *name, int ok) {
    api->write(ok ? "  ok    " : "  FAIL  ");
    write_line(api, name);
}

static int run_stress(fos_api_t *api) {
    fos_heap_info_t before, after;
    int fails = 0;
    int i;

    if (!api->mem_alloc || !api->mem_free || !api->get_heap_info) {
        write_line(api, "MEM: heap API missing — rebuild the kernel");
        return 1;
    }
    api->get_heap_info(&before);

    /* 1. many live blocks of mixed sizes, all holding their own pattern */
    for (i = 0; i < TEST_PTRS; i++) {
        sizes[i] = (rnd() % 8192u) + 1u;
        ptrs[i] = api->mem_alloc(sizes[i]);
        if (ptrs[i]) {
            fill(ptrs[i], sizes[i], (uint8_t)i);
        }
    }
    {
        int ok = 1;
        for (i = 0; i < TEST_PTRS; i++) {
            if (!ptrs[i] || !check(ptrs[i], sizes[i], (uint8_t)i)) {
                ok = 0;
            }
        }
        report(api, "256 mixed allocations do not overlap", ok);
        fails += !ok;
    }

    /* 2. free half, allocate into the holes, survivors must be untouched */
    for (i = 0; i < TEST_PTRS; i += 2) {
        api->mem_free(ptrs[i]);
        ptrs[i] = 0;
    }
    for (i = 0; i < TEST_PTRS; i += 2) {
        sizes[i] = (rnd() % 4096u) + 1u;
        ptrs[i] = api->mem_alloc(sizes[i]);
        if (ptrs[i]) {
            fill(ptrs[i], sizes[i], (uint8_t)(i + 7));
        }
    }
    {
        int ok = 1;
        for (i = 0; i < TEST_PTRS; i++) {
            uint8_t seed = (i % 2) ? (uint8_t)i : (uint8_t)(i + 7);
            if (!ptrs[i] || !check(ptrs[i], sizes[i], seed)) {
                ok = 0;
            }
        }
        report(api, "reuse of freed holes keeps neighbours intact", ok);
        fails += !ok;
    }

    /* 3. realloc preserves contents */
    {
        void *p = api->mem_alloc(64);
        int ok = p != 0;
        if (ok) {
            fill(p, 64, 0xA5);
            if (api->mem_realloc) {
                void *q = api->mem_realloc(p, 4096);
                ok = q && check(q, 64, 0xA5);
                api->mem_free(q);
            } else {
                api->mem_free(p);
            }
        }
        report(api, "realloc keeps the old contents", ok);
        fails += !ok;
    }

    /* 4. an allocation larger than any single block forces the pool to grow */
    {
        void *p = api->mem_alloc(4u * 1024u * 1024u);
        int ok = p != 0;
        if (ok) {
            fill(p, 4096, 0x5A);
            ok = check(p, 4096, 0x5A);
            api->mem_free(p);
        }
        report(api, "4 MB allocation succeeds and is writable", ok);
        fails += !ok;
    }

    /* 5. an impossible request fails instead of returning junk */
    {
        void *p = api->mem_alloc(1024ull * 1024ull * 1024ull);
        int ok = p == 0;
        if (!ok) {
            api->mem_free(p);
        }
        report(api, "1 GB request is refused", ok);
        fails += !ok;
    }

    /* 6. everything freed returns the heap to its starting size */
    for (i = 0; i < TEST_PTRS; i++) {
        api->mem_free(ptrs[i]);
        ptrs[i] = 0;
    }
    api->get_heap_info(&after);
    {
        int ok = after.heap_used == before.heap_used &&
                 after.heap_blocks == before.heap_blocks;
        report(api, "all blocks freed, heap back to baseline", ok);
        fails += !ok;
    }
    {
        int ok = after.pool_used <= before.pool_used;
        report(api, "empty page runs returned to the pool", ok);
        fails += !ok;
    }

    write_line(api, "");
    write_line(api, fails ? "RESULT: FAIL" : "RESULT: PASS");
    return fails;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    fos_mem_info_t info;
    const char *arg = skip_ws(api->cmdline);

    if (arg[0] == 't' || (arg[0] == '-' && arg[1] == 't')) {
        write_line(api, "FOS heap stress test");
        write_line(api, "");
        run_stress(api);
        return;
    }

    /* Deliberately exits holding memory: run "mem" afterwards and the heap
     * should be back at zero, reclaimed by the loader. */
    if (arg[0] == 'l') {
        for (int i = 0; i < 8; i++) {
            if (!api->mem_alloc || !api->mem_alloc(512u * 1024u)) {
                write_line(api, "MEM: allocation failed");
                return;
            }
        }
        write_line(api, "Leaked 4 MB on purpose — it is the loader's problem now.");
        return;
    }

    if (!api->get_mem_info) {
        write_line(api, "MEM: memory API missing — rebuild the kernel");
        return;
    }

    if (api->get_mem_info(&info) != 0) {
        write_line(api, "MEM: no BIOS memory map (E820 unavailable)");
        return;
    }

    write_line(api, "FOS memory report");
    write_line(api, "");
    print_region_table(api, &info);
    print_summary(api, &info);
    print_kernel_map(api, &info);
    print_largest_usable(api, &info);
    print_heap(api);
}
