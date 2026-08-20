/*
 * video.c — Set display mode from SYSTEM.INI using Bochs/QEMU DISPI.
 *
 * QEMU bochs-display exposes VRAM at PCI BAR0 and DISPI registers in BAR2
 * (MMIO offset 0x500). Legacy I/O ports 0x1CE/0x1CF + 0xE0000000 only work
 * for older/ISA setups.
 */

#include "video.h"
#include "config.h"
#include "console.h"
#include "string.h"

#define DISPI_REG_ID           0x0000
#define DISPI_REG_XRES         0x0001
#define DISPI_REG_YRES         0x0002
#define DISPI_REG_BPP          0x0003
#define DISPI_REG_ENABLE       0x0004
#define DISPI_REG_VIRT_WIDTH   0x0006
#define DISPI_REG_VRAM_64K     0x000A

#define DISPI_ID5              0xB0C5
#define DISPI_ENABLED          0x0001
#define DISPI_LFB_ENABLED      0x0040

#define PCI_CONFIG_ADDRESS     0xCF8
#define PCI_CONFIG_DATA        0xCFC

#define PCI_VENDOR_BOCHS       0x1234
#define PCI_DEVICE_BOCHS_VBE   0x1111
#define PCI_VENDOR_QEMU        0x1b36
#define PCI_DEVICE_QEMU_VGA    0x0100

#define PCI_VGA_BOCHS_OFFSET   0x500

#define FB_PHYS_LEGACY         0xE0000000ULL
#define FB_VIRT_ADDR           0x20000000ULL
#define MMIO_VIRT_ADDR         0x21000000ULL
#define PD_ADDR                0x7000ULL

#define FB_MIN_W 640
#define FB_MIN_H 480
#define FB_MAX_W 2560
#define FB_MAX_H 1440

/* bochs-display defaults to 16 MiB of VRAM; a 32bpp mode must fit. */
#define FB_VRAM_DEFAULT (16u * 1024u * 1024u)

typedef struct {
    const char *name;
    const char *alias;
    uint16_t width;
    uint16_t height;
} video_preset_t;

typedef struct {
    int found;
    uint64_t fb_phys;
    uint64_t mmio_phys;
} pci_bochs_t;

static const video_preset_t video_presets[] = {
    { "480p",  "640x480",  640,  480 },
    { "svga",  "800x600",  800,  600 },
    { "xga",   "1024x768", 1024, 768 },
    { "720p",  "1280x720", 1280, 720 },
    { "1080p", "1920x1080", 1920, 1080 },
    { "1200p", "1920x1200", 1920, 1200 },
    { "1440p", "2560x1440", 2560, 1440 },
};

static video_mode_t current_mode;
static int fb_active;
static volatile uint16_t *dispi_regs;
static uint64_t fb_phys_addr;

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint32_t value, uint16_t port) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFCu);
    outl(addr, PCI_CONFIG_ADDRESS);
    return inl(PCI_CONFIG_DATA);
}

static int pci_is_bochs(uint16_t vendor, uint16_t device) {
    return (vendor == PCI_VENDOR_BOCHS && device == PCI_DEVICE_BOCHS_VBE) ||
           (vendor == PCI_VENDOR_QEMU && device == PCI_DEVICE_QEMU_VGA);
}

static uint64_t pci_bar_address(uint8_t slot, uint8_t func, uint8_t bar_off) {
    uint32_t lo = pci_read32(0, slot, func, bar_off);

    if (lo == 0 || lo == 0xFFFFFFFFu) {
        return 0;
    }
    if (lo & 1u) {
        return 0;
    }

    uint64_t addr = lo & ~0xFULL;
    if ((lo & 0x06u) == 0x04u) {
        uint32_t hi = pci_read32(0, slot, func, bar_off + 4);
        addr |= (uint64_t)hi << 32;
    }
    return addr;
}

static pci_bochs_t pci_find_bochs(void) {
    pci_bochs_t info;

    info.found = 0;
    info.fb_phys = 0;
    info.mmio_phys = 0;

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_read32(0, slot, 0, 0);
        uint16_t vendor;
        uint16_t device;

        if (id == 0xFFFFFFFFu) {
            continue;
        }

        vendor = (uint16_t)(id & 0xFFFFu);
        device = (uint16_t)(id >> 16);
        if (!pci_is_bochs(vendor, device)) {
            continue;
        }

        info.found = 1;
        info.fb_phys = pci_bar_address(slot, 0, 0x10);
        info.mmio_phys = pci_bar_address(slot, 0, 0x18);
        return info;
    }

    return info;
}

#define PTE_PRESENT_RW_2M 0x83ULL
#define PTE_UNCACHED      0x18ULL /* PCD | PWT */

static void map_page_flags(uint64_t virt, uint64_t phys, uint64_t flags) {
    volatile uint64_t *pd = (volatile uint64_t *)PD_ADDR;
    uint64_t idx = (virt >> 21) & 0x1FF;

    if (idx >= 512) {
        return;
    }
    pd[idx] = (phys & ~0x1FFFFFULL) | PTE_PRESENT_RW_2M | flags;
}

static void map_page(uint64_t virt, uint64_t phys) {
    map_page_flags(virt, phys, 0);
}

static void flush_tlb(void) {
    __asm__ volatile(
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        ::: "rax", "memory");
}

static void map_range(uint64_t virt, uint64_t phys, uint32_t bytes) {
    uint64_t pages = ((uint64_t)bytes + 0x1FFFFFULL) >> 21;

    for (uint64_t i = 0; i < pages; i++) {
        map_page(virt + (i << 21), phys + (i << 21));
    }
    flush_tlb();
}

/* The legacy 0x1CE/0x1CF pair is index/data, but the PCI MMIO aperture exposes
 * the same registers as a flat array of 16-bit words (reg N at BAR2+0x500+N*2). */
static void dispi_write(uint16_t reg, uint16_t value) {
    if (dispi_regs) {
        dispi_regs[reg] = value;
        return;
    }
    outw(0x1CE, reg);
    outw(0x1CF, value);
}

static uint16_t dispi_read(uint16_t reg) {
    if (dispi_regs) {
        return dispi_regs[reg];
    }
    outw(0x1CE, reg);
    return inw(0x1CF);
}

static int dispi_present(void) {
    return dispi_read(DISPI_REG_ID) == DISPI_ID5;
}

static int resolution_valid(uint16_t w, uint16_t h) {
    return w >= FB_MIN_W && w <= FB_MAX_W && h >= FB_MIN_H && h <= FB_MAX_H;
}

static int parse_resolution(const char *mode, uint16_t *w, uint16_t *h) {
    const char *x;
    int width;
    int height;
    size_t i;

    if (!mode || !mode[0]) {
        return -1;
    }

    x = 0;
    for (i = 0; mode[i]; i++) {
        if (mode[i] == 'x' || mode[i] == 'X') {
            x = mode + i;
            break;
        }
    }
    if (!x || x == mode) {
        return -1;
    }

    width = atoi(mode);
    height = atoi(x + 1);
    if (width <= 0 || height <= 0) {
        return -1;
    }

    *w = (uint16_t)width;
    *h = (uint16_t)height;
    return resolution_valid(*w, *h) ? 0 : -1;
}

static int is_text_mode(const char *mode) {
    return strcasecmp(mode, "text") == 0 ||
           strcasecmp(mode, "vga") == 0 ||
           strcasecmp(mode, "80x25") == 0;
}

static int lookup_preset(const char *mode, uint16_t *w, uint16_t *h) {
    size_t i;

    for (i = 0; i < sizeof(video_presets) / sizeof(video_presets[0]); i++) {
        const video_preset_t *p = &video_presets[i];
        if (strcmp(mode, p->name) == 0 ||
            strcasecmp(mode, p->name) == 0 ||
            (p->alias && (strcmp(mode, p->alias) == 0 || strcasecmp(mode, p->alias) == 0))) {
            *w = p->width;
            *h = p->height;
            return 0;
        }
    }
    return -1;
}

static int setup_bochs_hw(pci_bochs_t *pci) {
    dispi_regs = 0;
    fb_phys_addr = FB_PHYS_LEGACY;

    if (pci->found && pci->fb_phys != 0) {
        fb_phys_addr = pci->fb_phys;
    }

    if (pci->found && pci->mmio_phys != 0) {
        map_page_flags(MMIO_VIRT_ADDR, pci->mmio_phys, PTE_UNCACHED);
        flush_tlb();
        dispi_regs = (volatile uint16_t *)(MMIO_VIRT_ADDR + PCI_VGA_BOCHS_OFFSET);
        if (dispi_present()) {
            return 1;
        }
        dispi_regs = 0;
    }

    /* Older/ISA setups only answer on the legacy index/data ports. */
    return dispi_present();
}

static int set_mode_framebuffer(uint16_t width, uint16_t height) {
    pci_bochs_t pci = pci_find_bochs();

    if (!setup_bochs_hw(&pci)) {
        dispi_regs = 0;
        return -1;
    }

    uint32_t vram = (uint32_t)dispi_read(DISPI_REG_VRAM_64K) * 64U * 1024U;

    if (vram == 0) {
        vram = FB_VRAM_DEFAULT;
    }
    if ((uint32_t)width * height * 4U > vram) {
        boot_line("[boot] mode needs more VRAM than the display has "
                  "(try bochs-display,vgamem=64M) — staying on VGA text");
        return -1;
    }

    dispi_write(DISPI_REG_ENABLE, 0);
    dispi_write(DISPI_REG_XRES, width);
    dispi_write(DISPI_REG_YRES, height);
    dispi_write(DISPI_REG_BPP, 32);
    dispi_write(DISPI_REG_VIRT_WIDTH, width);
    dispi_write(DISPI_REG_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    /* DISPI silently clamps modes it cannot do; trust the readback, not the request. */
    if (dispi_read(DISPI_REG_XRES) != width || dispi_read(DISPI_REG_YRES) != height) {
        dispi_write(DISPI_REG_ENABLE, 0);
        boot_line("[boot] display rejected the requested mode — staying on VGA text");
        return -1;
    }

    map_range(FB_VIRT_ADDR, fb_phys_addr, (uint32_t)width * height * 4U);

    current_mode.width = width;
    current_mode.height = height;
    current_mode.pitch = (uint32_t)width * 4U;
    current_mode.bpp = 32;
    current_mode.fb_addr = FB_VIRT_ADDR;
    fb_active = 1;

    console_init_framebuffer(&current_mode);
    return 0;
}

static int try_set_mode(const char *mode) {
    uint16_t w;
    uint16_t h;

    if (is_text_mode(mode)) {
        return 0;
    }

    if (lookup_preset(mode, &w, &h) == 0 || parse_resolution(mode, &w, &h) == 0) {
        if (set_mode_framebuffer(w, h) == 0) {
            return 0;
        }
        boot_line("[boot] framebuffer mode unavailable — staying on VGA text (use -device bochs-display)");
        return -1;
    }

    boot_line("[boot] Unknown video mode — see README or SYSTEM.INI for valid settings");
    return -1;
}

int video_init_from_config(void) {
    const char *mode = config_get("video", "mode");

    fb_active = 0;
    dispi_regs = 0;
    current_mode.width = 0;

    if (!mode || !mode[0]) {
        return 0;
    }

    return try_set_mode(mode);
}

int video_is_framebuffer(void) {
    return fb_active;
}

const video_mode_t *video_current_mode(void) {
    return fb_active ? &current_mode : 0;
}
