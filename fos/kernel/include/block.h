#pragma once

#include "types.h"

#define BLOCK_SECTOR_SIZE 512

typedef struct {
    uint8_t bus;
    uint8_t drive;
    uint64_t sector_count; /* from ATA IDENTIFY, 0 if unknown */
    char model[41];
} block_dev_t;

int  block_init(void);
int  block_count(void);
const block_dev_t *block_device(int index);
int  block_read(const block_dev_t *dev, uint64_t lba, void *buf);
int  block_write(const block_dev_t *dev, uint64_t lba, const void *buf);
uint8_t block_boot_bios_drive(void);
uint64_t block_dev_sectors(int phys_index);
const char *block_dev_model(int phys_index);

/* Maps physical block device index to logical drive letter index.
 * Boot disk is always logical drive 0. */
int block_logical_for_phys(int phys_index);
int block_phys_for_logical(int logical);
