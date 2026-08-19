/*
 * cpu.c — CPU identification via the CPUID instruction.
 */

#include "cpu.h"
#include "console.h"
#include "string.h"

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(sub));
}

static void brand_string(char *out, size_t sz) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_ext;

    cpuid(0x80000000, 0, &max_ext, &ebx, &ecx, &edx);
    if (max_ext < 0x80000004) {
        strncpy(out, "(no brand string)", sz);
        out[sz - 1] = 0;
        return;
    }

    uint32_t *dest = (uint32_t *)out;
    cpuid(0x80000002, 0, &eax, &ebx, &ecx, &edx);
    dest[0] = eax;
    dest[1] = ebx;
    dest[2] = ecx;
    dest[3] = edx;
    cpuid(0x80000003, 0, &eax, &ebx, &ecx, &edx);
    dest[4] = eax;
    dest[5] = ebx;
    dest[6] = ecx;
    dest[7] = edx;
    cpuid(0x80000004, 0, &eax, &ebx, &ecx, &edx);
    dest[8] = eax;
    dest[9] = ebx;
    dest[10] = ecx;
    dest[11] = edx;
    out[48] = 0;

    /* Trim leading spaces from brand string */
    char *p = out;
    while (*p == ' ') {
        p++;
    }
    if (p != out) {
        strcpy(out, p);
    }
}

void cpu_print_info(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    char brand[49];

    cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    *(uint32_t *)(vendor + 0) = ebx;
    *(uint32_t *)(vendor + 4) = edx;
    *(uint32_t *)(vendor + 8) = ecx;
    vendor[12] = 0;

    brand_string(brand, sizeof(brand));

    console_write_line("  CPU");
    console_write("    Vendor:       ");
    console_write_line(vendor);
    console_write("    Brand:        ");
    console_write_line(brand);

    cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    uint32_t family = ((eax >> 20) & 0xFF) + ((eax >> 8) & 0x0F);
    if (((eax >> 8) & 0x0F) == 0x0F) {
        family += (eax >> 20) & 0xFF;
    }
    uint32_t model = ((eax >> 12) & 0xF0) | ((eax >> 4) & 0x0F);
    if (((eax >> 8) & 0x0F) == 0x0F) {
        model |= (eax >> 12) & 0xF0;
    }
    uint32_t stepping = eax & 0xF;

    console_write("    Family/Model: ");
    console_write_dec(family);
    console_write(" / ");
    console_write_dec(model);
    console_write(", stepping ");
    console_write_dec(stepping);
    console_putchar('\n');

    cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
    console_write("    Long mode:    ");
    console_write_line((edx & (1u << 29)) ? "yes" : "no");

    console_write("    Cores (log):  ");
    console_write_dec(((ebx >> 16) & 0xFF));
    console_putchar('\n');
}
