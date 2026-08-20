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

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    fos_mem_info_t info;

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
}
