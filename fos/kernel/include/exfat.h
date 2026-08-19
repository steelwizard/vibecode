#pragma once

#include "types.h"

typedef struct {
    int block_index;
    uint32_t lba_start;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t fat_begin_lba;
    uint32_t fat_length_sectors;
    uint32_t cluster_begin_lba;
    uint32_t root_cluster;
    uint32_t total_clusters;
} exfat_vol_t;

int  exfat_mount(exfat_vol_t *vol, int block_index, uint32_t part_lba);
void exfat_unmount(exfat_vol_t *vol);

typedef int (*exfat_dir_cb)(const char *name, uint8_t attr, uint64_t size, void *ctx);
int exfat_list_dir(exfat_vol_t *vol, uint32_t cluster, exfat_dir_cb cb, void *ctx);
int exfat_find_path(exfat_vol_t *vol, uint32_t start_cluster, const char *path,
                    uint32_t *out_cluster, int *is_dir, uint64_t *file_size);
int exfat_read_file(exfat_vol_t *vol, uint32_t cluster, uint64_t offset,
                    void *buf, uint32_t size, uint64_t file_size);

#define EXFAT_ATTR_DIR 0x10
