#pragma once

#include "types.h"
#include "block.h"

#define PART_TYPE_FAT32_CHS 0x0B
#define PART_TYPE_FAT32_LBA 0x0C
#define PART_TYPE_EXFAT     0x07

typedef struct {
    uint8_t  type;
    uint32_t lba_start;
    uint32_t sector_count;
} partition_t;

#define MAX_PARTITIONS 4

int part_scan(const block_dev_t *dev, partition_t *out, int max_out);
