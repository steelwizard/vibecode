#pragma once

#include "types.h"

#define VFS_MAX_DRIVES 4
#define VFS_PATH_MAX   260

typedef enum {
    VFS_FS_NONE = 0,
    VFS_FS_FAT32,
    VFS_FS_EXFAT
} vfs_fs_type_t;

int  vfs_init(void);
int  vfs_drive_count(void);
vfs_fs_type_t vfs_drive_type(int drive);
int  vfs_get_drive(void);
int  vfs_set_drive(int drive);
const char *vfs_get_cwd(void);
int  vfs_set_cwd(const char *path);
int  vfs_resolve(int drive, const char *path, int *out_drive, char *out_path, size_t out_sz);
int  vfs_list_dir(int drive, const char *path);
int  vfs_read_file(int drive, const char *path, char *buf, size_t buf_sz, size_t *out_len);
int  vfs_write_file(int drive, const char *path, const void *data, size_t len);
int  vfs_mkdir(int drive, const char *path);
int  vfs_delete(int drive, const char *path);
int  vfs_copy(int drive, const char *src, const char *dst);
int  vfs_is_dir(int drive, const char *path);

#define VFS_DIR_MAX 128

typedef struct {
    char     name[64];
    int      is_dir;
    uint32_t size;
} vfs_dirent_t;

typedef struct {
    vfs_dirent_t entries[VFS_DIR_MAX];
    int          count;
} vfs_dir_list_t;

int vfs_read_dir(int drive, const char *path, vfs_dir_list_t *out);

void vfs_format_prompt(char *buf, size_t sz);
void vfs_print_drive_table(void);

#define VFS_COMPLETE_MAX 32

typedef struct {
    char names[VFS_COMPLETE_MAX][64];
    int  count;
} vfs_complete_result_t;

/* Tab completion: partial path/name -> matching directory entries. */
int vfs_complete(int drive, const char *partial, vfs_complete_result_t *out);
