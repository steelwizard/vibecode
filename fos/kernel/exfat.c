#include "exfat.h"
#include "block.h"
#include "string.h"

static uint8_t sector_buf[512];

static int read_sector(int block_index, uint32_t lba) {
    const block_dev_t *dev = block_device(block_index);
    if (!dev) {
        return -1;
    }
    return block_read(dev, lba, sector_buf);
}

static int read_sector_vol(const exfat_vol_t *vol, uint32_t lba) {
    return read_sector(vol->block_index, lba);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint32_t cluster_lba(const exfat_vol_t *vol, uint32_t cluster) {
    return vol->cluster_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
}

static uint32_t exfat_next_cluster(const exfat_vol_t *vol, uint32_t cluster, int no_fat_chain) {
    if (no_fat_chain) {
        return cluster + 1;
    }
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_begin_lba + fat_offset / vol->bytes_per_sector;
    uint32_t ent_off = fat_offset % vol->bytes_per_sector;
    if (read_sector_vol(vol, fat_sector) != 0) {
        return 0xFFFFFFFF;
    }
    return rd32(sector_buf + ent_off);
}

int exfat_mount(exfat_vol_t *vol, int block_index, uint32_t part_lba) {
    vol->block_index = block_index;
    if (read_sector(block_index, part_lba) != 0) {
        return -1;
    }
    if (memcmp(sector_buf + 3, "EXFAT   ", 8) != 0) {
        return -1;
    }

    uint8_t bps_shift = sector_buf[108];
    uint8_t spc_shift = sector_buf[109];
    if (bps_shift > 12 || spc_shift > 25) {
        return -1;
    }

    vol->lba_start = part_lba;
    vol->bytes_per_sector = 1u << bps_shift;
    vol->sectors_per_cluster = 1u << spc_shift;
    vol->fat_begin_lba = part_lba + rd32(sector_buf + 80);
    vol->fat_length_sectors = rd32(sector_buf + 84);
    vol->cluster_begin_lba = part_lba + rd32(sector_buf + 88);
    vol->root_cluster = rd32(sector_buf + 96);
    vol->total_clusters = rd32(sector_buf + 92);

    if (vol->bytes_per_sector != 512) {
        return -1;
    }
    (void)vol->fat_length_sectors;
    return 0;
}

void exfat_unmount(exfat_vol_t *vol) {
    (void)vol;
}

/* Append UTF-16LE code units as ASCII into dst (already NUL-terminated). */
static void utf16le_append_ascii(const uint16_t *src, int chars, char *dst, size_t sz) {
    size_t n = strlen(dst);
    for (int i = 0; i < chars && n + 1 < sz; i++) {
        uint16_t ch = src[i];
        if (ch < 128) {
            dst[n++] = (char)ch;
        } else {
            dst[n++] = '?';
        }
    }
    dst[n] = 0;
}

static int walk_dir_cluster(exfat_vol_t *vol, uint32_t cluster, exfat_dir_cb cb, void *ctx) {
    while (cluster >= 2 && cluster < 0xFFFFFFF8) {
        uint32_t lba = cluster_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            int i = 0;
            while (i + 32 <= 512) {
                uint8_t *e = sector_buf + i;
                uint8_t type = e[0];
                if (type == 0x00) {
                    return 0;
                }
                if (type == 0x85) {
                    uint8_t secondary = e[1];
                    uint16_t attrs = (uint16_t)e[4] | ((uint16_t)e[5] << 8);
                    i += 32;
                    char name[256];
                    name[0] = 0;
                    uint64_t size = 0;
                    uint32_t first_cluster = 0;
                    int got = 0;
                    for (int sec = 0; sec < secondary && i + 32 <= 512; sec++) {
                        uint8_t *se = sector_buf + i;
                        if (se[0] == 0xC0) {
                            size = rd64(se + 8);
                            first_cluster = rd32(se + 20);
                            got |= 1;
                        } else if (se[0] == 0xC1) {
                            uint8_t nchars = se[1];
                            utf16le_append_ascii((const uint16_t *)(se + 2), nchars,
                                                 name, sizeof(name));
                            got |= 2;
                        }
                        i += 32;
                    }
                    if (got == 3 && name[0]) {
                        (void)first_cluster;
                        if (cb(name, (uint8_t)attrs, size, ctx) != 0) {
                            return 0;
                        }
                    }
                    continue;
                }
                i += 32;
            }
        }
        cluster = exfat_next_cluster(vol, cluster, 0);
    }
    return 0;
}

int exfat_list_dir(exfat_vol_t *vol, uint32_t cluster, exfat_dir_cb cb, void *ctx) {
    return walk_dir_cluster(vol, cluster, cb, ctx);
}

static int match_component(const char *name, const char *want) {
    return strcasecmp(name, want) == 0;
}

static int find_in_dir(exfat_vol_t *vol, uint32_t cluster, const char *comp,
                       uint32_t *out_cluster, uint8_t *out_attr, uint64_t *out_size,
                       int *out_no_fat_chain) {
    while (cluster >= 2 && cluster < 0xFFFFFFF8) {
        uint32_t lba = cluster_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            int i = 0;
            while (i + 32 <= 512) {
                uint8_t *e = sector_buf + i;
                if (e[0] == 0x00) {
                    return -1;
                }
                if (e[0] == 0x85) {
                    uint8_t secondary = e[1];
                    uint16_t attrs = (uint16_t)e[4] | ((uint16_t)e[5] << 8);
                    i += 32;
                    char name[256];
                    name[0] = 0;
                    uint64_t size = 0;
                    uint32_t first_cluster = 0;
                    int no_fat = 0;
                    int got = 0;
                    for (int sec = 0; sec < secondary && i + 32 <= 512; sec++) {
                        uint8_t *se = sector_buf + i;
                        if (se[0] == 0xC0) {
                            /* GeneralSecondaryFlags bit 1 = NoFatChain */
                            no_fat = (se[1] & 0x02) ? 1 : 0;
                            size = rd64(se + 8);
                            first_cluster = rd32(se + 20);
                            got |= 1;
                        } else if (se[0] == 0xC1) {
                            uint8_t nchars = se[1];
                            utf16le_append_ascii((const uint16_t *)(se + 2), nchars,
                                                 name, sizeof(name));
                            got |= 2;
                        }
                        i += 32;
                    }
                    if (got == 3 && match_component(name, comp)) {
                        *out_cluster = first_cluster;
                        *out_attr = (uint8_t)attrs;
                        *out_size = size;
                        if (out_no_fat_chain) {
                            *out_no_fat_chain = no_fat;
                        }
                        return 0;
                    }
                    continue;
                }
                i += 32;
            }
        }
        cluster = exfat_next_cluster(vol, cluster, 0);
    }
    return -1;
}

int exfat_find_path(exfat_vol_t *vol, uint32_t start_cluster, const char *path,
                    uint32_t *out_cluster, int *is_dir, uint64_t *file_size,
                    int *no_fat_chain) {
    uint32_t cluster = start_cluster;
    char comp[256];
    const char *p = path;
    int last_no_fat = 0;

    if (no_fat_chain) {
        *no_fat_chain = 0;
    }

    while (*p == '\\') {
        p++;
    }
    if (*p == 0) {
        *out_cluster = cluster;
        *is_dir = 1;
        *file_size = 0;
        return 0;
    }

    for (;;) {
        while (*p == '\\') {
            p++;
        }
        if (*p == 0) {
            *out_cluster = cluster;
            *is_dir = 1;
            *file_size = 0;
            return 0;
        }

        size_t n = 0;
        while (*p && *p != '\\' && n + 1 < sizeof(comp)) {
            comp[n++] = *p++;
        }
        comp[n] = 0;

        uint32_t next = 0;
        uint8_t attr = 0;
        uint64_t size = 0;
        int no_fat = 0;
        if (find_in_dir(vol, cluster, comp, &next, &attr, &size, &no_fat) != 0) {
            return -1;
        }

        cluster = next;
        last_no_fat = no_fat;
        if (*p == 0) {
            *out_cluster = cluster;
            *is_dir = (attr & EXFAT_ATTR_DIR) != 0;
            *file_size = size;
            if (no_fat_chain) {
                *no_fat_chain = last_no_fat;
            }
            return 0;
        }
        if (!(attr & EXFAT_ATTR_DIR)) {
            return -1;
        }
    }
}

int exfat_read_file(exfat_vol_t *vol, uint32_t cluster, uint64_t offset,
                    void *buf, uint32_t size, uint64_t file_size, int no_fat_chain) {
    if (offset >= file_size) {
        return 0;
    }
    if (offset + size > file_size) {
        size = (uint32_t)(file_size - offset);
    }

    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    uint64_t skip_clusters = offset / cluster_size;
    uint32_t skip_bytes = (uint32_t)(offset % cluster_size);

    while (skip_clusters > 0) {
        cluster = exfat_next_cluster(vol, cluster, no_fat_chain);
        if (cluster >= 0xFFFFFFF8) {
            return -1;
        }
        skip_clusters--;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = size;

    if (skip_bytes > 0) {
        uint32_t lba = cluster_lba(vol, cluster);
        if (read_sector_vol(vol, lba + skip_bytes / vol->bytes_per_sector) != 0) {
            return -1;
        }
        uint32_t byte_in_sector = skip_bytes % vol->bytes_per_sector;
        uint32_t avail = vol->bytes_per_sector - byte_in_sector;
        if (avail > remaining) {
            avail = remaining;
        }
        memcpy(out, sector_buf + byte_in_sector, avail);
        out += avail;
        remaining -= avail;
        skip_bytes += avail;
        if (skip_bytes >= cluster_size) {
            cluster = exfat_next_cluster(vol, cluster, no_fat_chain);
        }
    }

    while (remaining > 0) {
        uint32_t lba = cluster_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster && remaining > 0; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            uint32_t n = remaining > vol->bytes_per_sector ? vol->bytes_per_sector : remaining;
            memcpy(out, sector_buf, n);
            out += n;
            remaining -= n;
        }
        if (remaining > 0) {
            cluster = exfat_next_cluster(vol, cluster, no_fat_chain);
            if (cluster >= 0xFFFFFFF8) {
                break;
            }
        }
    }

    return (int)size;
}
