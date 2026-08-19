#pragma once

#include "types.h"

/* Fixed kernel API block — .COM programs call these instead of linking drivers. */

#define FOS_API_ADDR   0x0000000000FF0000ULL
#define FOS_API_MAGIC  0x49534F46u /* 'FOSI' */

typedef enum {
    FOS_KEY_NONE = 0,
    FOS_KEY_CHAR,
    FOS_KEY_ENTER,
    FOS_KEY_BACKSPACE,
    FOS_KEY_DELETE,
    FOS_KEY_TAB,
    FOS_KEY_UP,
    FOS_KEY_DOWN,
    FOS_KEY_LEFT,
    FOS_KEY_RIGHT,
    FOS_KEY_HOME,
    FOS_KEY_END,
    FOS_KEY_PAGEUP,
    FOS_KEY_PAGEDOWN
} fos_key_type_t;

typedef struct {
    fos_key_type_t type;
    char           ch;
} fos_key_event_t;

typedef struct {
    char     name[64];
    int      is_dir;
    uint32_t size;
} fos_dirent_t;

#define FOS_DIR_MAX 128

typedef struct {
    fos_dirent_t entries[FOS_DIR_MAX];
    int          count;
} fos_dir_t;

typedef struct {
    uint32_t magic;
    void (*write)(const char *s);
    void (*write_n)(const char *s, size_t n);
    void (*write_line)(const char *s);
    void (*putchar)(char c);
    char     cmdline[256];
    char     pipe_in[4096];
    size_t   pipe_in_len;
    /* Extended API for interactive programs (editor, etc.) */
    int (*has_key)(void);
    fos_key_event_t (*read_key)(void);
    int (*read_file)(const char *path, char *buf, size_t cap, size_t *out_len);
    int (*write_file)(const char *path, const char *buf, size_t len);
    void (*clear_screen)(void);
    int (*get_drive)(void);
    void (*goto_xy)(int x, int y);
    int (*read_dir)(const char *path, fos_dir_t *out);
    int (*set_cwd)(const char *path);
    void (*get_cwd)(char *buf, size_t cap);
    int (*run_com)(const char *path, const char *args);
    void (*set_color)(uint8_t fg, uint8_t bg);
    void (*write_color)(uint8_t fg, uint8_t bg, const char *s);
} fos_api_t;

void fos_api_init(void);
void fos_api_set_cmdline(const char *args);
void fos_api_set_pipe(const char *data, size_t len);
void fos_api_clear_pipe(void);
