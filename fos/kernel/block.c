/*
 * block.c — ATA PIO disk I/O and physical/logical drive mapping.
 *
 * Probes primary (0x1F0) and secondary (0x170) ATA buses via IDENTIFY.
 * The BIOS boot drive (from stage1 at 0x7DFF) is always logical drive 0.
 */

#include "block.h"
#include "string.h"

#define BOOT_DRIVE_ADDR 0x7DFF
#define MAX_PHYS_DEVS   4

typedef struct {
    uint16_t base;
    uint16_t ctrl;
} ata_bus_t;

static const ata_bus_t buses[2] = {
    { 0x1F0, 0x3F6 },
    { 0x170, 0x376 }
};

static block_dev_t phys_devs[MAX_PHYS_DEVS];
static int phys_count;
static int logical_map[MAX_PHYS_DEVS];
static int boot_phys_index = -1;
static uint8_t boot_bios;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static void ata_wait_bsy(const ata_bus_t *bus) {
    for (int i = 0; i < 4000000; i++) {
        if ((inb(bus->ctrl) & 0x80) == 0) {
            return;
        }
    }
}

static void ata_reset(const ata_bus_t *bus) {
    outb(bus->ctrl, 0x04);
    for (volatile int i = 0; i < 100000; i++) {
    }
    outb(bus->ctrl, 0x00);
    ata_wait_bsy(bus);
}

static int ata_wait_drq(const ata_bus_t *bus) {
    for (int i = 0; i < 4000000; i++) {
        uint8_t st = inb(bus->base + 7);
        if (st & 0x08) {
            return 1;
        }
        if (st & 0x01) {
            return 0;
        }
    }
    return 0;
}

static int ata_identify_drive(uint8_t bus_idx, uint8_t drive_sel, block_dev_t *dev) {
    const ata_bus_t *bus = &buses[bus_idx];
    uint16_t buf[256];

    outb(bus->ctrl, 0);
    ata_wait_bsy(bus);
    outb(bus->base + 6, 0xA0 | (drive_sel << 4));
    for (int i = 0; i < 4; i++) {
        inb(bus->base + 7);
    }
    outb(bus->base + 7, 0xEC);
    uint8_t st = inb(bus->base + 7);
    if (st == 0) {
        return 0;
    }
    if (!ata_wait_drq(bus)) {
        return 0;
    }

    for (int i = 0; i < 256; i++) {
        buf[i] = inw(bus->base);
    }

    dev->bus = bus_idx;
    dev->drive = drive_sel;
    dev->sector_count = 0;

    /* Words 27-46: model name (byte-swapped pairs) */
    for (int i = 0; i < 20; i++) {
        dev->model[i * 2] = (char)(buf[27 + i] >> 8);
        dev->model[i * 2 + 1] = (char)(buf[27 + i] & 0xFF);
    }
    dev->model[40] = 0;
    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && dev->model[i] == ' '; i--) {
        dev->model[i] = 0;
    }

    /* LBA48 total sectors in words 100-103, else LBA28 in words 60-61 */
    uint64_t lba48 =
        ((uint64_t)buf[100]) |
        ((uint64_t)buf[101] << 16) |
        ((uint64_t)buf[102] << 32) |
        ((uint64_t)buf[103] << 48);
    if (lba48 != 0) {
        dev->sector_count = lba48;
    } else {
        dev->sector_count = ((uint64_t)buf[61] << 16) | buf[60];
    }

    return 1;
}

uint8_t block_boot_bios_drive(void) {
    return boot_bios;
}

int block_count(void) {
    return phys_count;
}

const block_dev_t *block_device(int index) {
    if (index < 0 || index >= phys_count) {
        return NULL;
    }
    return &phys_devs[index];
}

int block_logical_for_phys(int phys_index) {
    if (phys_index < 0 || phys_index >= phys_count) {
        return -1;
    }
    return logical_map[phys_index];
}

int block_phys_for_logical(int logical) {
    for (int i = 0; i < phys_count; i++) {
        if (logical_map[i] == logical) {
            return i;
        }
    }
    return -1;
}

uint64_t block_dev_sectors(int phys_index) {
    if (phys_index < 0 || phys_index >= phys_count) {
        return 0;
    }
    return phys_devs[phys_index].sector_count;
}

const char *block_dev_model(int phys_index) {
    if (phys_index < 0 || phys_index >= phys_count) {
        return "";
    }
    return phys_devs[phys_index].model;
}

int block_read(const block_dev_t *dev, uint64_t lba, void *buf) {
    if (!dev || lba > 0x0FFFFFFF) {
        return -1;
    }

    const ata_bus_t *bus = &buses[dev->bus];
    uint16_t *wbuf = (uint16_t *)buf;

    for (int attempt = 0; attempt < 3; attempt++) {
        ata_wait_bsy(bus);
        outb(bus->base + 6, 0xA0 | (dev->drive << 4));
        ata_wait_bsy(bus);
        outb(bus->base + 6, 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F));
        outb(bus->base + 2, 1);
        outb(bus->base + 3, (uint8_t)lba);
        outb(bus->base + 4, (uint8_t)(lba >> 8));
        outb(bus->base + 5, (uint8_t)(lba >> 16));
        outb(bus->base + 7, 0x20);

        if (!ata_wait_drq(bus)) {
            continue;
        }

        for (int i = 0; i < 256; i++) {
            wbuf[i] = inw(bus->base);
        }
        return 0;
    }
    return -1;
}

int block_write(const block_dev_t *dev, uint64_t lba, const void *buf) {
    if (!dev || lba > 0x0FFFFFFF) {
        return -1;
    }

    const ata_bus_t *bus = &buses[dev->bus];
    const uint16_t *wbuf = (const uint16_t *)buf;

    for (int attempt = 0; attempt < 3; attempt++) {
        ata_wait_bsy(bus);
        outb(bus->base + 6, 0xA0 | (dev->drive << 4));
        ata_wait_bsy(bus);
        outb(bus->base + 6, 0xE0 | (dev->drive << 4) | ((lba >> 24) & 0x0F));
        outb(bus->base + 2, 1);
        outb(bus->base + 3, (uint8_t)lba);
        outb(bus->base + 4, (uint8_t)(lba >> 8));
        outb(bus->base + 5, (uint8_t)(lba >> 16));
        outb(bus->base + 7, 0x30);

        if (!ata_wait_drq(bus)) {
            continue;
        }

        for (int i = 0; i < 256; i++) {
            outw(bus->base, wbuf[i]);
        }
        return 0;
    }
    return -1;
}

int block_init(void) {
    boot_bios = *(volatile uint8_t *)BOOT_DRIVE_ADDR;
    phys_count = 0;
    boot_phys_index = -1;

    for (uint8_t b = 0; b < 2; b++) {
        ata_reset(&buses[b]);
        for (uint8_t d = 0; d < 2; d++) {
            block_dev_t candidate;
            memset(&candidate, 0, sizeof(candidate));
            if (!ata_identify_drive(b, d, &candidate)) {
                continue;
            }
            if (phys_count >= MAX_PHYS_DEVS) {
                return phys_count;
            }
            phys_devs[phys_count] = candidate;
            if (b == 0 && d == 0) {
                boot_phys_index = phys_count;
            }
            phys_count++;
        }
    }

    /* Boot disk is always logical drive 0. */
    int next_logical = 1;
    for (int i = 0; i < phys_count; i++) {
        logical_map[i] = -1;
    }
    if (boot_phys_index >= 0) {
        logical_map[boot_phys_index] = 0;
    }
    for (int i = 0; i < phys_count; i++) {
        if (logical_map[i] < 0) {
            logical_map[i] = next_logical++;
        }
    }

    return phys_count;
}
