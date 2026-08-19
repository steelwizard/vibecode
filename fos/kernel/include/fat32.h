#pragma once

#include "types.h"

typedef struct {
    int block_index;
    uint32_t lba_start;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_cluster;
    uint32_t fat_begin_lba;
    uint32_t data_begin_lba;
    uint32_t total_clusters;
} fat32_vol_t;

int  fat32_mount(fat32_vol_t *vol, int block_index, uint32_t part_lba);
void fat32_unmount(fat32_vol_t *vol);

typedef int (*fat32_dir_cb)(const char *name, uint8_t attr, uint32_t size, void *ctx);
int fat32_list_dir(fat32_vol_t *vol, uint32_t cluster, fat32_dir_cb cb, void *ctx);
int fat32_find_path(fat32_vol_t *vol, uint32_t start_cluster, const char *path,
                    uint32_t *out_cluster, int *is_dir);
int fat32_read_file(fat32_vol_t *vol, uint32_t cluster, uint32_t offset,
                    void *buf, uint32_t size, uint32_t file_size);
int fat32_write_file(fat32_vol_t *vol, uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size);
int fat32_lookup(fat32_vol_t *vol, uint32_t dir_cluster, const char *name,
                 uint32_t *out_cluster, uint8_t *out_attr, uint32_t *out_size);
uint64_t fat32_free_bytes(const fat32_vol_t *vol);

#define FAT_ATTR_DIR 0x10
