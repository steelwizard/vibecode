#pragma once

#include "types.h"

/* Fixed kernel API block — .COM programs call these instead of linking drivers. */

#define FOS_API_ADDR   0x0000000000FF0000ULL
#define FOS_API_MAGIC  0x49534F46u /* 'FOSI' */

/* PIC IRQ lines (see irq_register / irq_enable). IRQ0 timer is always active. */
#define FOS_IRQ_TIMER     0
#define FOS_IRQ_KEYBOARD  1
#define FOS_IRQ_CASCADE   2
#define FOS_IRQ_COM2      3
#define FOS_IRQ_COM1      4
#define FOS_IRQ_LPT2      5 /* default Sound Blaster 16 IRQ */
#define FOS_IRQ_FLOPPY    6
#define FOS_IRQ_LPT1      7
#define FOS_IRQ_RTC       8
#define FOS_IRQ_FREE9     9
#define FOS_IRQ_FREE10    10
#define FOS_IRQ_FREE11    11
#define FOS_IRQ_MOUSE     12
#define FOS_IRQ_FPU       13
#define FOS_IRQ_ATA1      14
#define FOS_IRQ_ATA2      15
#define FOS_IRQ_SB16      FOS_IRQ_LPT2

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
    uint64_t base;
    uint64_t length;
    uint32_t type;
} fos_mem_region_t;

#define FOS_MEM_REGION_MAX 32

typedef struct {
    int              count;
    fos_mem_region_t regions[FOS_MEM_REGION_MAX];
    uint64_t         total_bytes;
    uint64_t         usable_bytes;
    uint64_t         reserved_bytes;
    uint64_t         kernel_base;
    uint64_t         kernel_size;
} fos_mem_info_t;

typedef struct {
    uint64_t pool_total;    /* page pool size */
    uint64_t pool_used;     /* pages handed to the heap */
    uint64_t heap_reserved; /* bytes the heap holds */
    uint64_t heap_used;     /* bytes handed out to callers */
    uint64_t heap_blocks;   /* live allocations */
} fos_heap_info_t;

typedef struct {
    uint16_t year;    /* 1970–2099 */
    uint8_t  month;   /* 1–12 */
    uint8_t  day;     /* 1–31 */
    uint8_t  hour;    /* 0–23 */
    uint8_t  minute;  /* 0–59 */
    uint8_t  second;  /* 0–59 */
    uint8_t  weekday; /* 1–7, 0 = unknown */
} fos_rtc_t;

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
    uint64_t (*get_ticks_ms)(void);
    void (*sleep_ms)(uint32_t ms);
    int (*rtc_read)(fos_rtc_t *out);
    int (*rtc_write)(const fos_rtc_t *in);
    int (*mkdir)(const char *path);
    int (*get_mem_info)(fos_mem_info_t *out);
    /* IRQ API — PIC lines 0–15 (FOS_IRQ_* in irq.h). Handlers run in interrupt context. */
    int (*irq_register)(uint8_t irq, void (*handler)(uint8_t irq));
    int (*irq_unregister)(uint8_t irq);
    void (*irq_enable)(uint8_t irq);
    void (*irq_disable)(uint8_t irq);
    uint32_t (*irq_pending)(void);
    void (*irq_clear)(uint8_t irq);
    int (*irq_in_handler)(void);
    int (*sound_present)(void);
    int (*sound_beep)(uint32_t freq_hz, uint32_t ms);
    int (*sound_play)(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz);
    void (*sound_stop)(void);
    int (*sound_playing)(void);
    int (*stat_file)(const char *path, uint32_t *size, int *is_dir);
    int (*read_at)(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *out_len);
    void (*begin_direct)(void);
    void (*end_direct)(void);
    int (*sound_start)(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz);
    int (*sound_can_queue)(void);
    /* Console text grid: 80x25 on VGA text, larger in framebuffer modes. */
    void (*get_term_size)(int *cols, int *rows);
    /* Heap. Anything still allocated when the program exits is reclaimed by
     * the loader, so freeing is good manners rather than a requirement. */
    void *(*mem_alloc)(size_t bytes);
    void (*mem_free)(void *ptr);
    void *(*mem_realloc)(void *ptr, size_t bytes);
    int (*get_heap_info)(fos_heap_info_t *out);
} fos_api_t;

void fos_api_init(void);
void fos_api_set_cmdline(const char *args);
void fos_api_set_pipe(const char *data, size_t len);
void fos_api_clear_pipe(void);
