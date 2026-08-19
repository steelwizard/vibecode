#include "vfs.h"
#include "block.h"
#include "partition.h"
#include "fat32.h"
#include "exfat.h"
#include "string.h"
#include "console.h"

typedef struct {
    int mounted;
    vfs_fs_type_t type;
    int block_index;
    uint32_t part_lba;
    uint32_t part_sectors;
    uint64_t free_bytes;
    union {
        fat32_vol_t fat32;
        exfat_vol_t exfat;
    } fs;
} drive_vol_t;

static drive_vol_t drives[VFS_MAX_DRIVES];
static int num_drives;
static int current_drive;
static char cwd[VFS_MAX_DRIVES][VFS_PATH_MAX];

static int try_mount_partition(int block_index, uint32_t lba, uint32_t sectors,
                               drive_vol_t *dv) {
    fat32_vol_t f;
    if (fat32_mount(&f, block_index, lba) == 0) {
        dv->type = VFS_FS_FAT32;
        dv->fs.fat32 = f;
        dv->block_index = block_index;
        dv->part_lba = lba;
        dv->part_sectors = sectors;
        dv->free_bytes = fat32_free_bytes(&f);
        dv->mounted = 1;
        return 0;
    }

    exfat_vol_t x;
    if (exfat_mount(&x, block_index, lba) == 0) {
        dv->type = VFS_FS_EXFAT;
        dv->fs.exfat = x;
        dv->block_index = block_index;
        dv->part_lba = lba;
        dv->part_sectors = sectors;
        dv->free_bytes = 0;
        dv->mounted = 1;
        return 0;
    }
    return -1;
}

int vfs_init(void) {
    memset(drives, 0, sizeof(drives));
    num_drives = 0;
    current_drive = 0;

    /* Probe ATA controllers; boot physical disk becomes logical 0: */
    block_init();

    for (int logical = 0; logical < VFS_MAX_DRIVES; logical++) {
        strcpy(cwd[logical], "\\");
        int phys = block_phys_for_logical(logical);
        if (phys < 0) {
            continue;
        }

        drives[num_drives].mounted = 0;
        drives[num_drives].type = VFS_FS_NONE;
        drives[num_drives].block_index = phys;
        drives[num_drives].part_lba = 0;
        drives[num_drives].part_sectors = 0;
        drives[num_drives].free_bytes = 0;

        const block_dev_t *dev = block_device(phys);
        partition_t parts[MAX_PARTITIONS];
        int pc = part_scan(dev, parts, MAX_PARTITIONS);

        int mounted = 0;
        for (int i = 0; i < pc && !mounted; i++) {
            if (parts[i].type == PART_TYPE_FAT32_CHS ||
                parts[i].type == PART_TYPE_FAT32_LBA ||
                parts[i].type == PART_TYPE_EXFAT) {
                if (try_mount_partition(phys, parts[i].lba_start, parts[i].sector_count,
                                      &drives[num_drives]) == 0) {
                    mounted = 1;
                }
            }
        }

        if (!mounted && logical == 0) {
            uint32_t fallback_lba = 2048;
            uint32_t fallback_sectors = (uint32_t)block_dev_sectors(phys);
            if (fallback_sectors > fallback_lba) {
                fallback_sectors -= fallback_lba;
            }
            if (try_mount_partition(phys, fallback_lba, fallback_sectors,
                                  &drives[num_drives]) == 0) {
                mounted = 1;
            }
        }

        if (!mounted && logical != 0) {
            uint32_t secs = (uint32_t)block_dev_sectors(phys);
            try_mount_partition(phys, 0, secs, &drives[num_drives]);
        }

        num_drives++;
    }

    if (num_drives == 0) {
        num_drives = 1;
        drives[0].block_index = 0;
    }

    return num_drives;
}

int vfs_drive_count(void) {
    return num_drives;
}

vfs_fs_type_t vfs_drive_type(int drive) {
    if (drive < 0 || drive >= num_drives) {
        return VFS_FS_NONE;
    }
    return drives[drive].mounted ? drives[drive].type : VFS_FS_NONE;
}

int vfs_get_drive(void) {
    return current_drive;
}

int vfs_set_drive(int drive) {
    if (drive < 0 || drive >= num_drives) {
        return -1;
    }
    current_drive = drive;
    return 0;
}

const char *vfs_get_cwd(void) {
    return cwd[current_drive];
}

static void normalize_path(char *out, const char *in) {
    path_normalize_slashes(out, in, VFS_PATH_MAX);
}

int vfs_set_cwd(const char *path) {
    char norm[VFS_PATH_MAX];
    normalize_path(norm, path);

    int drive = current_drive;
    char rel[VFS_PATH_MAX];
    if (vfs_resolve(drive, norm, &drive, rel, sizeof(rel)) != 0) {
        return -1;
    }

    uint32_t cluster;
    int is_dir;
    drive_vol_t *dv = &drives[drive];

    if (!dv->mounted) {
        return -1;
    }

    if (dv->type == VFS_FS_FAT32) {
        if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, rel,
                            &cluster, &is_dir) != 0 || !is_dir) {
            return -1;
        }
    } else if (dv->type == VFS_FS_EXFAT) {
        uint64_t dummy;
        if (exfat_find_path(&dv->fs.exfat, dv->fs.exfat.root_cluster, rel,
                            &cluster, &is_dir, &dummy) != 0 || !is_dir) {
            return -1;
        }
    } else {
        return -1;
    }

    current_drive = drive;
    strcpy(cwd[drive], rel);
    if (cwd[drive][0] != '\\') {
        char tmp[VFS_PATH_MAX];
        tmp[0] = '\\';
        strcpy(tmp + 1, cwd[drive]);
        strcpy(cwd[drive], tmp);
    }
    return 0;
}

int vfs_resolve(int drive, const char *path, int *out_drive, char *out_path, size_t out_sz) {
    char norm[VFS_PATH_MAX];
    normalize_path(norm, path);

    int d = drive;
    const char *p = norm;

    if (p[0] && p[1] == ':') {
        d = p[0] - '0';
        if (d < 0 || d >= num_drives) {
            return -1;
        }
        p += 2;
    }

    if (*p == 0) {
        strncpy(out_path, "\\", out_sz);
        out_path[out_sz - 1] = 0;
        *out_drive = d;
        return 0;
    }

    if (*p != '\\') {
        /* relative to cwd */
        char combined[VFS_PATH_MAX];
        if (strcmp(cwd[d], "\\") == 0) {
            combined[0] = '\\';
            strcpy(combined + 1, p);
        } else {
            strcpy(combined, cwd[d]);
            size_t n = strlen(combined);
            if (n + 1 + strlen(p) + 1 >= VFS_PATH_MAX) {
                return -1;
            }
            if (combined[n - 1] != '\\') {
                combined[n++] = '\\';
            }
            strcpy(combined + n, p);
        }
        path_normalize_slashes(out_path, combined, out_sz);
    } else {
        strncpy(out_path, p, out_sz);
        out_path[out_sz - 1] = 0;
    }

    /* collapse \.\ and \..\ — minimal: strip trailing backslash except root */
    *out_drive = d;
    return 0;
}

static int dir_entry_count = 0;

static int dir_cb_print(const char *name, uint8_t attr, uint32_t size, void *ctx) {
    (void)ctx;
    /* Skip volume label and hidden/system entries */
    if ((attr & 0x08) || (attr & 0x06)) {
        return 0;
    }
    dir_entry_count++;
    console_write("  ");
    console_write(name);
    if (attr & 0x10) {
        console_write_line("  <DIR>");
    } else {
        console_write("  ");
        /* simple decimal size */
        char buf[16];
        int i = 0;
        uint32_t v = size;
        if (v == 0) {
            buf[i++] = '0';
        } else {
            char tmp[16];
            int j = 0;
            while (v > 0) {
                tmp[j++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (j > 0) {
                buf[i++] = tmp[--j];
            }
        }
        buf[i] = 0;
        console_write_line(buf);
    }
    return 0;
}

static int exfat_dir_cb_print(const char *name, uint8_t attr, uint64_t size, void *ctx) {
    return dir_cb_print(name, attr, (uint32_t)size, ctx);
}

int vfs_list_dir(int drive, const char *path) {
    char resolved[VFS_PATH_MAX];
    int d = drive;
    if (vfs_resolve(drive, path, &d, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    drive_vol_t *dv = &drives[d];
    if (!dv->mounted) {
        return -1;
    }

    uint32_t cluster;
    int is_dir;
    dir_entry_count = 0;

    if (dv->type == VFS_FS_FAT32) {
        if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, resolved,
                            &cluster, &is_dir) != 0 || !is_dir) {
            return -1;
        }
        if (fat32_list_dir(&dv->fs.fat32, cluster, dir_cb_print, NULL) != 0) {
            return -1;
        }
        if (dir_entry_count == 0) {
            console_write_line("  (empty directory)");
        }
        return 0;
    }

    if (dv->type == VFS_FS_EXFAT) {
        uint64_t dummy;
        if (exfat_find_path(&dv->fs.exfat, dv->fs.exfat.root_cluster, resolved,
                            &cluster, &is_dir, &dummy) != 0 || !is_dir) {
            return -1;
        }
        if (exfat_list_dir(&dv->fs.exfat, cluster, exfat_dir_cb_print, NULL) != 0) {
            return -1;
        }
        if (dir_entry_count == 0) {
            console_write_line("  (empty directory)");
        }
        return 0;
    }

    return -1;
}

int vfs_read_file(int drive, const char *path, char *buf, size_t buf_sz, size_t *out_len) {
    char resolved[VFS_PATH_MAX];
    int d = drive;
    if (vfs_resolve(drive, path, &d, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    drive_vol_t *dv = &drives[d];
    if (!dv->mounted) {
        return -1;
    }

    /* Split parent path and filename */
    char parent[VFS_PATH_MAX];
    char file[64];
    strcpy(parent, resolved);
    char *last = parent;
    for (char *c = parent; *c; c++) {
        if (*c == '\\') {
            last = c + 1;
        }
    }
    if (last == parent && *parent == '\\' && parent[1] == 0) {
        return -1;
    }
    strcpy(file, last);
    if (last == parent + 1 && *parent == '\\') {
        parent[1] = 0;
    } else if (last > parent) {
        *(last - 1) = 0;
        if (parent[0] == 0) {
            parent[0] = '\\';
            parent[1] = 0;
        }
    }

    if (dv->type == VFS_FS_FAT32) {
        uint32_t dir_cluster;
        int is_dir;
        if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, parent,
                            &dir_cluster, &is_dir) != 0 || !is_dir) {
            return -1;
        }
        uint32_t cl;
        uint8_t attr;
        uint32_t fsize;
        if (fat32_lookup(&dv->fs.fat32, dir_cluster, file, &cl, &attr, &fsize) != 0) {
            return -1;
        }
        if (attr & FAT_ATTR_DIR) {
            return -1;
        }
        size_t to_read = fsize < buf_sz - 1 ? fsize : buf_sz - 1;
        if (fat32_read_file(&dv->fs.fat32, cl, 0, buf, (uint32_t)to_read, fsize) < 0) {
            return -1;
        }
        buf[to_read] = 0;
        if (out_len) {
            *out_len = to_read;
        }
        return 0;
    }

    if (dv->type == VFS_FS_EXFAT) {
        uint32_t dir_cluster;
        int is_dir;
        uint64_t dummy;
        if (exfat_find_path(&dv->fs.exfat, dv->fs.exfat.root_cluster, parent,
                            &dir_cluster, &is_dir, &dummy) != 0 || !is_dir) {
            return -1;
        }
        uint32_t cl;
        uint8_t attr;
        uint64_t fsize;
        char comp[256];
        strcpy(comp, file);
        if (exfat_find_path(&dv->fs.exfat, dir_cluster, comp, &cl, &is_dir, &fsize) != 0 || is_dir) {
            return -1;
        }
        (void)attr;
        size_t to_read = fsize < buf_sz - 1 ? (size_t)fsize : buf_sz - 1;
        if (exfat_read_file(&dv->fs.exfat, cl, 0, buf, (uint32_t)to_read, fsize) < 0) {
            return -1;
        }
        buf[to_read] = 0;
        if (out_len) {
            *out_len = to_read;
        }
        return 0;
    }

    return -1;
}

int vfs_write_file(int drive, const char *path, const void *data, size_t len) {
    char resolved[VFS_PATH_MAX];
    int d = drive;
    if (vfs_resolve(drive, path, &d, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    drive_vol_t *dv = &drives[d];
    if (!dv->mounted || dv->type != VFS_FS_FAT32) {
        return -1;
    }

    char parent[VFS_PATH_MAX];
    char file[64];
    strcpy(parent, resolved);
    char *last = parent;
    for (char *c = parent; *c; c++) {
        if (*c == '\\') {
            last = c + 1;
        }
    }
    if (last == parent && *parent == '\\' && parent[1] == 0) {
        return -1;
    }
    strcpy(file, last);
    if (last == parent + 1 && *parent == '\\') {
        parent[1] = 0;
    } else if (last > parent) {
        *(last - 1) = 0;
        if (parent[0] == 0) {
            parent[0] = '\\';
            parent[1] = 0;
        }
    }

    uint32_t dir_cluster;
    int is_dir;
    if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, parent,
                        &dir_cluster, &is_dir) != 0 || !is_dir) {
        return -1;
    }

    return fat32_write_file(&dv->fs.fat32, dir_cluster, file, data, (uint32_t)len);
}

int vfs_is_dir(int drive, const char *path) {
    char resolved[VFS_PATH_MAX];
    int d = drive;
    if (vfs_resolve(drive, path, &d, resolved, sizeof(resolved)) != 0) {
        return 0;
    }
    drive_vol_t *dv = &drives[d];
    if (!dv->mounted) {
        return 0;
    }
    uint32_t cluster;
    int is_dir;
    if (dv->type == VFS_FS_FAT32) {
        if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, resolved,
                            &cluster, &is_dir) != 0) {
            return 0;
        }
        return is_dir;
    }
    if (dv->type == VFS_FS_EXFAT) {
        uint64_t dummy;
        if (exfat_find_path(&dv->fs.exfat, dv->fs.exfat.root_cluster, resolved,
                            &cluster, &is_dir, &dummy) != 0) {
            return 0;
        }
        return is_dir;
    }
    return 0;
}

void vfs_format_prompt(char *buf, size_t sz) {
    const char *p = cwd[current_drive];
    if (strcmp(p, "\\") == 0) {
        /* 0:\> */
        buf[0] = (char)('0' + current_drive);
        buf[1] = ':';
        buf[2] = '\\';
        buf[3] = '>';
        buf[4] = ' ';
        buf[5] = 0;
    } else {
        size_t n = 0;
        buf[n++] = (char)('0' + current_drive);
        buf[n++] = ':';
        const char *s = p;
        while (*s && n + 2 < sz) {
            buf[n++] = *s++;
        }
        buf[n++] = '>';
        buf[n++] = ' ';
        buf[n] = 0;
    }
}

static const char *fs_type_name(vfs_fs_type_t t) {
    switch (t) {
    case VFS_FS_FAT32: return "FAT32";
    case VFS_FS_EXFAT: return "exFAT";
    default:           return "—";
    }
}

static void write_padded(const char *s, int width) {
    int n = 0;
    while (s[n] && n < width) {
        console_putchar(s[n++]);
    }
    while (n++ < width) {
        console_putchar(' ');
    }
}

typedef struct {
    const char *prefix;
    vfs_complete_result_t *out;
} complete_ctx_t;

static int complete_cb(const char *name, uint8_t attr, uint32_t size, void *ctx) {
    (void)size;
    complete_ctx_t *c = (complete_ctx_t *)ctx;
    vfs_complete_result_t *out = c->out;

    if ((attr & 0x08) || (attr & 0x06)) {
        return 0;
    }
    if (out->count >= VFS_COMPLETE_MAX) {
        return 0;
    }

    size_t plen = strlen(c->prefix);
    if (plen > 0 && strncasecmp(name, c->prefix, plen) != 0) {
        return 0;
    }

    strncpy(out->names[out->count], name, 63);
    out->names[out->count][63] = 0;
    if (attr & 0x10) {
        size_t n = strlen(out->names[out->count]);
        if (n + 1 < 64) {
            out->names[out->count][n] = '\\';
            out->names[out->count][n + 1] = 0;
        }
    }
    out->count++;
    return 0;
}

static int exfat_complete_cb(const char *name, uint8_t attr, uint64_t size, void *ctx) {
    return complete_cb(name, attr, (uint32_t)size, ctx);
}

static int list_dir_for_complete(int drive, const char *dir_path, const char *prefix,
                                 vfs_complete_result_t *out) {
    char resolved[VFS_PATH_MAX];
    int d = drive;
    if (vfs_resolve(drive, dir_path, &d, resolved, sizeof(resolved)) != 0) {
        return -1;
    }

    drive_vol_t *dv = &drives[d];
    if (!dv->mounted) {
        return -1;
    }

    uint32_t cluster;
    int is_dir;
    complete_ctx_t ctx = {prefix, out};

    if (dv->type == VFS_FS_FAT32) {
        if (fat32_find_path(&dv->fs.fat32, dv->fs.fat32.root_cluster, resolved,
                            &cluster, &is_dir) != 0 || !is_dir) {
            return -1;
        }
        return fat32_list_dir(&dv->fs.fat32, cluster, complete_cb, &ctx);
    }

    if (dv->type == VFS_FS_EXFAT) {
        uint64_t dummy;
        if (exfat_find_path(&dv->fs.exfat, dv->fs.exfat.root_cluster, resolved,
                            &cluster, &is_dir, &dummy) != 0 || !is_dir) {
            return -1;
        }
        return exfat_list_dir(&dv->fs.exfat, cluster, exfat_complete_cb, &ctx);
    }

    return -1;
}

int vfs_complete(int drive, const char *partial, vfs_complete_result_t *out) {
    char dir[VFS_PATH_MAX];
    char prefix[64];
    const char *slash = 0;

    out->count = 0;
    if (!partial) {
        partial = "";
    }

    for (const char *p = partial; *p; p++) {
        if (*p == '\\') {
            slash = p;
        }
    }

    if (slash) {
        size_t dlen = (size_t)(slash - partial);
        if (dlen >= sizeof(dir)) {
            return -1;
        }
        memcpy(dir, partial, dlen);
        dir[dlen] = 0;
        strncpy(prefix, slash + 1, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = 0;
    } else {
        strcpy(dir, vfs_get_cwd());
        strncpy(prefix, partial, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = 0;
    }

    if (dir[0] == 0) {
        dir[0] = '\\';
        dir[1] = 0;
    }

    return list_dir_for_complete(drive, dir, prefix, out);
}

void vfs_print_drive_table(void) {
    console_write_line("  Volumes (df)");
    console_write_line("  Drive  Type    Size        Free        Use%   Mount");
    console_write_line("  -----  ----    ----        ----        ----   -----");

    for (int i = 0; i < num_drives; i++) {
        drive_vol_t *dv = &drives[i];
        uint64_t total = 0;
        if (dv->part_sectors > 0) {
            total = (uint64_t)dv->part_sectors * BLOCK_SECTOR_SIZE;
        } else {
            total = block_dev_sectors(dv->block_index) * BLOCK_SECTOR_SIZE;
        }

        console_write("  ");
        console_putchar((char)('0' + i));
        console_write(":     ");

        write_padded(fs_type_name(dv->mounted ? dv->type : VFS_FS_NONE), 6);
        console_write("  ");

        if (total > 0) {
            console_write_size(total);
        } else {
            console_write("—");
        }
        console_write("  ");

        if (dv->free_bytes > 0) {
            console_write_size(dv->free_bytes);
        } else {
            console_write("—");
        }
        console_write("  ");

        if (dv->free_bytes > 0 && total > 0) {
            uint64_t used = total - dv->free_bytes;
            uint64_t pct = used * 100 / total;
            console_write_dec(pct);
            console_write("%");
        } else {
            console_write("—");
        }
        console_write("   ");
        console_putchar((char)('0' + i));
        console_write_line(":\\");
    }
}
