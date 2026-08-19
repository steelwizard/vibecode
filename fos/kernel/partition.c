#include "partition.h"
#include "block.h"
#include "string.h"

int part_scan(const block_dev_t *dev, partition_t *out, int max_out) {
    if (!dev || !out || max_out <= 0) {
        return 0;
    }

    uint8_t mbr[512];
    if (block_read(dev, 0, mbr) != 0) {
        return 0;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < 4 && count < max_out; i++) {
        uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0) {
            continue;
        }
        uint32_t lba =
            (uint32_t)e[8] |
            ((uint32_t)e[9] << 8) |
            ((uint32_t)e[10] << 16) |
            ((uint32_t)e[11] << 24);
        uint32_t sectors =
            (uint32_t)e[12] |
            ((uint32_t)e[13] << 8) |
            ((uint32_t)e[14] << 16) |
            ((uint32_t)e[15] << 24);
        if (sectors == 0) {
            continue;
        }
        out[count].type = type;
        out[count].lba_start = lba;
        out[count].sector_count = sectors;
        count++;
    }
    return count;
}
