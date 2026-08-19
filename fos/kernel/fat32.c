#include "fat32.h"
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

static int read_sector_vol(const fat32_vol_t *vol, uint32_t lba) {
    return read_sector(vol->block_index, lba);
}

static int write_sector_vol(const fat32_vol_t *vol, uint32_t lba) {
    const block_dev_t *dev = block_device(vol->block_index);
    if (!dev) {
        return -1;
    }
    return block_write(dev, lba, sector_buf);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t cluster_lba(const fat32_vol_t *vol, uint32_t cluster) {
    return vol->data_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
}

static uint32_t fat32_next_cluster(const fat32_vol_t *vol, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_begin_lba + fat_offset / vol->bytes_per_sector;
    uint32_t ent_off = fat_offset % vol->bytes_per_sector;
    if (read_sector_vol(vol, fat_sector) != 0) {
        return 0x0FFFFFFF;
    }
    uint32_t next =
        *(uint32_t *)(sector_buf + ent_off) & 0x0FFFFFFF;
    return next;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat32_mount(fat32_vol_t *vol, int block_index, uint32_t part_lba) {
    vol->block_index = block_index;
    if (read_sector(block_index, part_lba) != 0) {
        return -1;
    }

    if (memcmp(sector_buf + 82, "FAT32   ", 8) != 0 &&
        memcmp(sector_buf + 82, "FAT     ", 8) != 0) {
        return -1;
    }

    vol->lba_start = part_lba;
    vol->bytes_per_sector = rd16(sector_buf + 11);
    vol->sectors_per_cluster = sector_buf[13];
    vol->reserved_sectors = rd16(sector_buf + 14);
    vol->num_fats = sector_buf[16];
    vol->fat_size_sectors = rd32(sector_buf + 36);
    vol->root_cluster = rd32(sector_buf + 44);

    if (vol->bytes_per_sector != 512 || vol->sectors_per_cluster == 0 ||
        vol->num_fats == 0) {
        return -1;
    }

    vol->fat_begin_lba = part_lba + vol->reserved_sectors;
    vol->data_begin_lba = vol->fat_begin_lba + vol->num_fats * vol->fat_size_sectors;
    vol->total_clusters = vol->fat_size_sectors * (vol->bytes_per_sector / 4);
    return 0;
}

void fat32_unmount(fat32_vol_t *vol) {
    (void)vol;
}

/* Read FSInfo sector for free cluster count (returns 0 if unknown). */
uint64_t fat32_free_bytes(const fat32_vol_t *vol) {
    if (vol->reserved_sectors < 2) {
        return 0;
    }
    if (read_sector_vol(vol, vol->lba_start + 1) != 0) {
        return 0;
    }
    if (rd32(sector_buf + 0) != 0x41615252) {
        return 0;
    }
    uint32_t free_count = rd32(sector_buf + 0x1E8);
    if (free_count == 0xFFFFFFFF || free_count == 0) {
        return 0;
    }
    return (uint64_t)free_count * vol->sectors_per_cluster * vol->bytes_per_sector;
}

static void format_83_name(const uint8_t *raw, char *out, size_t sz) {
    char base[9];
    char ext[4];
    int i;
    for (i = 0; i < 8; i++) {
        base[i] = raw[i];
    }
    base[8] = 0;
    for (i = 7; i >= 0 && base[i] == ' '; i--) {
        base[i] = 0;
    }
    for (i = 0; i < 3; i++) {
        ext[i] = raw[8 + i];
    }
    ext[3] = 0;
    for (i = 2; i >= 0 && ext[i] == ' '; i--) {
        ext[i] = 0;
    }

    if (ext[0]) {
        size_t n = 0;
        for (i = 0; base[i] && n + 1 < (int)sz; i++) {
            out[n++] = base[i];
        }
        if (n + 1 < sz) {
            out[n++] = '.';
        }
        for (i = 0; ext[i] && n + 1 < (int)sz; i++) {
            out[n++] = ext[i];
        }
        out[n] = 0;
    } else {
        strncpy(out, base, sz);
        out[sz - 1] = 0;
    }
}

static int walk_dir_cluster(fat32_vol_t *vol, uint32_t cluster, fat32_dir_cb cb, void *ctx) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_lba(vol, cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            for (int i = 0; i < 512; i += 32) {
                uint8_t *e = sector_buf + i;
                if (e[0] == 0) {
                    return 0;
                }
                if (e[0] == 0xE5 || e[11] == 0x0F) {
                    continue;
                }
                char name[13];
                format_83_name(e, name, sizeof(name));
                uint32_t size = rd32(e + 28);
                uint32_t cl = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                uint8_t attr = e[11];
                (void)cl;
                if (cb(name, attr, size, ctx) != 0) {
                    return 0;
                }
            }
        }
        cluster = fat32_next_cluster(vol, cluster);
    }
    return 0;
}

int fat32_list_dir(fat32_vol_t *vol, uint32_t cluster, fat32_dir_cb cb, void *ctx) {
    return walk_dir_cluster(vol, cluster, cb, ctx);
}

static int match_component(const char *name, const char *want) {
    return strcasecmp(name, want) == 0;
}

int fat32_find_path(fat32_vol_t *vol, uint32_t start_cluster, const char *path,
                    uint32_t *out_cluster, int *is_dir) {
    uint32_t cluster = start_cluster;
    char comp[64];
    const char *p = path;

    while (*p == '\\') {
        p++;
    }

    if (*p == 0) {
        *out_cluster = cluster;
        *is_dir = 1;
        return 0;
    }

    for (;;) {
        while (*p == '\\') {
            p++;
        }
        if (*p == 0) {
            *out_cluster = cluster;
            *is_dir = 1;
            return 0;
        }

        size_t n = 0;
        while (*p && *p != '\\' && n + 1 < sizeof(comp)) {
            comp[n++] = *p++;
        }
        comp[n] = 0;

        int found = 0;
        uint32_t next_cluster = 0;
        uint8_t attr = 0;
        uint32_t scan = cluster;

        while (scan >= 2 && scan < 0x0FFFFFF8 && !found) {
            uint32_t lba = cluster_lba(vol, scan);
            for (uint32_t s = 0; s < vol->sectors_per_cluster && !found; s++) {
                if (read_sector_vol(vol, lba + s) != 0) {
                    return -1;
                }
                for (int i = 0; i < 512; i += 32) {
                    uint8_t *e = sector_buf + i;
                    if (e[0] == 0) {
                        scan = 0x0FFFFFF8;
                        break;
                    }
                    if (e[0] == 0xE5 || e[11] == 0x0F) {
                        continue;
                    }
                    char name[13];
                    format_83_name(e, name, sizeof(name));
                    if (match_component(name, comp)) {
                        next_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                        attr = e[11];
                        found = 1;
                        break;
                    }
                }
            }
            if (!found && scan < 0x0FFFFFF8) {
                scan = fat32_next_cluster(vol, scan);
            }
        }

        if (!found) {
            return -1;
        }

        cluster = next_cluster;
        if (*p == 0) {
            *out_cluster = cluster;
            *is_dir = (attr & FAT_ATTR_DIR) != 0;
            return 0;
        }
        if (!(attr & FAT_ATTR_DIR)) {
            return -1;
        }
    }
}

int fat32_lookup(fat32_vol_t *vol, uint32_t dir_cluster, const char *name,
                 uint32_t *out_cluster, uint8_t *out_attr, uint32_t *out_size) {
    uint32_t scan = dir_cluster;
    while (scan >= 2 && scan < 0x0FFFFFF8) {
        uint32_t lba = cluster_lba(vol, scan);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            for (int i = 0; i < 512; i += 32) {
                uint8_t *e = sector_buf + i;
                if (e[0] == 0) {
                    return -1;
                }
                if (e[0] == 0xE5 || e[11] == 0x0F) {
                    continue;
                }
                char nm[13];
                format_83_name(e, nm, sizeof(nm));
                if (match_component(nm, name)) {
                    *out_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                    *out_attr = e[11];
                    *out_size = rd32(e + 28);
                    return 0;
                }
            }
        }
        scan = fat32_next_cluster(vol, scan);
    }
    return -1;
}

int fat32_read_file(fat32_vol_t *vol, uint32_t cluster, uint32_t offset,
                    void *buf, uint32_t size, uint32_t file_size) {
    if (offset >= file_size) {
        return 0;
    }
    if (offset + size > file_size) {
        size = file_size - offset;
    }

    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;

    while (skip_clusters > 0) {
        cluster = fat32_next_cluster(vol, cluster);
        if (cluster >= 0x0FFFFFF8) {
            return -1;
        }
        skip_clusters--;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t remaining = size;

    if (skip_bytes > 0) {
        uint32_t lba = cluster_lba(vol, cluster);
        uint32_t sector_in_cluster = skip_bytes / vol->bytes_per_sector;
        uint32_t byte_in_sector = skip_bytes % vol->bytes_per_sector;
        if (read_sector_vol(vol, lba + sector_in_cluster) != 0) {
            return -1;
        }
        uint32_t avail = vol->bytes_per_sector - byte_in_sector;
        if (avail > remaining) {
            avail = remaining;
        }
        memcpy(out, sector_buf + byte_in_sector, avail);
        out += avail;
        remaining -= avail;
        skip_bytes += avail;
        if (skip_bytes >= cluster_size) {
            cluster = fat32_next_cluster(vol, cluster);
            skip_bytes = 0;
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
            cluster = fat32_next_cluster(vol, cluster);
            if (cluster >= 0x0FFFFFF8) {
                break;
            }
        }
    }

    return (int)size;
}

static void fat32_set_cluster(fat32_vol_t *vol, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = vol->fat_begin_lba + fat_offset / vol->bytes_per_sector;
    uint32_t ent_off = fat_offset % vol->bytes_per_sector;
    if (read_sector_vol(vol, fat_sector) != 0) {
        return;
    }
    uint32_t old = rd32(sector_buf + ent_off);
    wr32(sector_buf + ent_off, (old & 0xF0000000) | (value & 0x0FFFFFFF));
    write_sector_vol(vol, fat_sector);
    if (vol->num_fats > 1) {
        write_sector_vol(vol, fat_sector + vol->fat_size_sectors);
    }
}

static uint32_t fat32_alloc_cluster(fat32_vol_t *vol) {
    for (uint32_t c = 2; c < vol->total_clusters + 2; c++) {
        if (fat32_next_cluster(vol, c) == 0) {
            fat32_set_cluster(vol, c, 0x0FFFFFFF);
            return c;
        }
    }
    return 0;
}

static void fat32_free_chain(fat32_vol_t *vol, uint32_t cluster) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat32_next_cluster(vol, cluster);
        fat32_set_cluster(vol, cluster, 0);
        cluster = next;
    }
}

static void name_to_83(const char *name, uint8_t out[11]) {
    char base[9];
    char ext[4];
    int i;
    for (i = 0; i < 8; i++) {
        base[i] = ' ';
    }
    base[8] = 0;
    for (i = 0; i < 3; i++) {
        ext[i] = ' ';
    }
    ext[3] = 0;

    const char *dot = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    int bi = 0;
    for (const char *p = name; *p && p != dot && bi < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        if (c == '\\' || c == '/' || c == ':') {
            continue;
        }
        base[bi++] = c;
    }

    int ei = 0;
    if (dot) {
        for (const char *p = dot + 1; *p && ei < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 32);
            }
            ext[ei++] = c;
        }
    }

    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t)base[i];
    }
    for (i = 0; i < 3; i++) {
        out[8 + i] = (uint8_t)ext[i];
    }
}

static int fat32_write_cluster_chain(fat32_vol_t *vol, uint32_t cluster,
                                     const uint8_t *data, uint32_t size) {
    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    uint32_t offset = 0;
    uint32_t cur = cluster;

    while (offset < size) {
        uint32_t lba = cluster_lba(vol, cur);
        for (uint32_t s = 0; s < vol->sectors_per_cluster && offset < size; s++) {
            uint32_t n = size - offset;
            if (n > vol->bytes_per_sector) {
                n = vol->bytes_per_sector;
            }
            memset(sector_buf, 0, vol->bytes_per_sector);
            memcpy(sector_buf, data + offset, n);
            if (write_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            offset += n;
        }
        if (offset < size) {
            uint32_t next = fat32_next_cluster(vol, cur);
            if (next >= 0x0FFFFFF8) {
                next = fat32_alloc_cluster(vol);
                if (next < 2) {
                    return -1;
                }
                fat32_set_cluster(vol, cur, next);
            }
            cur = next;
        }
    }
    (void)cluster_size;
    return 0;
}

static int fat32_find_dir_slot(fat32_vol_t *vol, uint32_t dir_cluster,
                               uint32_t *out_lba, int *out_off) {
    while (dir_cluster >= 2 && dir_cluster < 0x0FFFFFF8) {
        uint32_t lba = cluster_lba(vol, dir_cluster);
        for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
            if (read_sector_vol(vol, lba + s) != 0) {
                return -1;
            }
            for (int i = 0; i < 512; i += 32) {
                uint8_t *e = sector_buf + i;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    *out_lba = lba + s;
                    *out_off = i;
                    return 0;
                }
            }
        }
        dir_cluster = fat32_next_cluster(vol, dir_cluster);
    }
    return -1;
}

int fat32_write_file(fat32_vol_t *vol, uint32_t dir_cluster, const char *name,
                     const void *data, uint32_t size) {
    uint32_t cluster = 0;
    uint8_t attr = 0;
    uint32_t old_size = 0;

    if (fat32_lookup(vol, dir_cluster, name, &cluster, &attr, &old_size) == 0) {
        if (attr & FAT_ATTR_DIR) {
            return -1;
        }
        fat32_free_chain(vol, cluster);
    } else {
        cluster = 0;
    }

    if (size == 0) {
        uint32_t slot_lba;
        int slot_off;

        if (cluster != 0) {
            uint32_t scan = dir_cluster;
            while (scan >= 2 && scan < 0x0FFFFFF8) {
                uint32_t lba = cluster_lba(vol, scan);
                for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
                    if (read_sector_vol(vol, lba + s) != 0) {
                        return -1;
                    }
                    for (int i = 0; i < 512; i += 32) {
                        uint8_t *e = sector_buf + i;
                        if (e[0] == 0 || e[0] == 0xE5) {
                            continue;
                        }
                        char nm[13];
                        format_83_name(e, nm, sizeof(nm));
                        if (match_component(nm, name)) {
                            wr16(e + 26, 0);
                            wr16(e + 20, 0);
                            wr32(e + 28, 0);
                            return write_sector_vol(vol, lba + s);
                        }
                    }
                }
                scan = fat32_next_cluster(vol, scan);
            }
            return -1;
        }

        if (fat32_find_dir_slot(vol, dir_cluster, &slot_lba, &slot_off) != 0) {
            return -1;
        }
        if (read_sector_vol(vol, slot_lba) != 0) {
            return -1;
        }
        uint8_t *e = sector_buf + slot_off;
        name_to_83(name, e);
        e[11] = 0x20;
        wr16(e + 26, 0);
        wr16(e + 20, 0);
        wr32(e + 28, 0);
        return write_sector_vol(vol, slot_lba);
    }

    uint32_t first = fat32_alloc_cluster(vol);
    if (first < 2) {
        return -1;
    }
    fat32_set_cluster(vol, first, 0x0FFFFFF8);

    uint32_t cluster_size = vol->sectors_per_cluster * vol->bytes_per_sector;
    uint32_t needed = (size + cluster_size - 1) / cluster_size;
    uint32_t cur = first;
    for (uint32_t i = 1; i < needed; i++) {
        uint32_t next = fat32_alloc_cluster(vol);
        if (next < 2) {
            fat32_free_chain(vol, first);
            return -1;
        }
        fat32_set_cluster(vol, cur, next);
        fat32_set_cluster(vol, next, 0x0FFFFFF8);
        cur = next;
    }

    if (fat32_write_cluster_chain(vol, first, (const uint8_t *)data, size) != 0) {
        fat32_free_chain(vol, first);
        return -1;
    }

    if (cluster != 0) {
        uint32_t scan = dir_cluster;
        while (scan >= 2 && scan < 0x0FFFFFF8) {
            uint32_t lba = cluster_lba(vol, scan);
            for (uint32_t s = 0; s < vol->sectors_per_cluster; s++) {
                if (read_sector_vol(vol, lba + s) != 0) {
                    return -1;
                }
                for (int i = 0; i < 512; i += 32) {
                    uint8_t *e = sector_buf + i;
                    if (e[0] == 0 || e[0] == 0xE5) {
                        continue;
                    }
                    char nm[13];
                    format_83_name(e, nm, sizeof(nm));
                    if (match_component(nm, name)) {
                        wr32(e + 28, size);
                        wr16(e + 26, first & 0xFFFF);
                        wr16(e + 20, (uint16_t)(first >> 16));
                        return write_sector_vol(vol, lba + s);
                    }
                }
            }
            scan = fat32_next_cluster(vol, scan);
        }
        return -1;
    }

    uint32_t slot_lba;
    int slot_off;
    if (fat32_find_dir_slot(vol, dir_cluster, &slot_lba, &slot_off) != 0) {
        fat32_free_chain(vol, first);
        return -1;
    }
    if (read_sector_vol(vol, slot_lba) != 0) {
        fat32_free_chain(vol, first);
        return -1;
    }
    uint8_t *e = sector_buf + slot_off;
    name_to_83(name, e);
    e[11] = 0x20;
    wr16(e + 26, first & 0xFFFF);
    wr16(e + 20, (uint16_t)(first >> 16));
    wr32(e + 28, size);
    return write_sector_vol(vol, slot_lba);
}
