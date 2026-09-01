/*
 * fos_api.c — Kernel API block at fixed address 0xFF0000 for .COM programs.
 *
 * User programs don't link against the kernel; they call through this struct.
 * The shell sets cmdline and pipe_in before exec_run() jumps to the program.
 */

#include "fos_api.h"
#include "console.h"
#include "keyboard.h"
#include "vfs.h"
#include "foscom.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"
#include "memory.h"
#include "irq.h"
#include "sb16.h"
#include "heap.h"
#include "env.h"
#include "shell.h"
#include "mouse.h"

#define api ((fos_api_t *)FOS_API_ADDR)

/* Frozen .COM ABI offsets. Insert new function pointers at the end of
 * fos_api_t, never in the middle — and never grow pipe_in[]. */
#define fos_offsetof(type, member) ((size_t)(uintptr_t)&((type *)0)->member)
_Static_assert(fos_offsetof(fos_api_t, magic) == 0, "fos_api ABI");
_Static_assert(fos_offsetof(fos_api_t, write) == 8, "fos_api ABI");
_Static_assert(fos_offsetof(fos_api_t, cmdline) == 40, "fos_api ABI");
_Static_assert(fos_offsetof(fos_api_t, pipe_in) == 296, "fos_api ABI");
_Static_assert(sizeof(((fos_api_t *)0)->pipe_in) == 4096, "fos_api ABI");
_Static_assert(fos_offsetof(fos_api_t, get_mem_info) == 4544, "fos_api ABI");
_Static_assert(FOS_O_READ == VFS_O_READ && FOS_O_WRITE == VFS_O_WRITE &&
                   FOS_O_CREATE == VFS_O_CREATE && FOS_O_TRUNC == VFS_O_TRUNC &&
                   FOS_O_APPEND == VFS_O_APPEND,
               "fopen flags");

static fos_key_event_t api_read_key(void) {
    key_event_t ev = keyboard_read_event();
    fos_key_event_t out = {FOS_KEY_NONE, 0};

    switch (ev.type) {
    case KEY_CHAR:
        out.type = FOS_KEY_CHAR;
        out.ch = ev.ch;
        break;
    case KEY_ENTER:
        out.type = FOS_KEY_ENTER;
        break;
    case KEY_BACKSPACE:
        out.type = FOS_KEY_BACKSPACE;
        break;
    case KEY_DELETE:
        out.type = FOS_KEY_DELETE;
        break;
    case KEY_TAB:
        out.type = FOS_KEY_TAB;
        break;
    case KEY_UP:
        out.type = FOS_KEY_UP;
        break;
    case KEY_DOWN:
        out.type = FOS_KEY_DOWN;
        break;
    case KEY_LEFT:
        out.type = FOS_KEY_LEFT;
        break;
    case KEY_RIGHT:
        out.type = FOS_KEY_RIGHT;
        break;
    case KEY_HOME:
        out.type = FOS_KEY_HOME;
        break;
    case KEY_END:
        out.type = FOS_KEY_END;
        break;
    case KEY_PAGEUP:
        out.type = FOS_KEY_PAGEUP;
        break;
    case KEY_PAGEDOWN:
        out.type = FOS_KEY_PAGEDOWN;
        break;
    default:
        break;
    }
    return out;
}

static int api_read_file(const char *path, char *buf, size_t cap, size_t *out_len) {
    vfs_file_t f;
    uint32_t got = 0;
    uint32_t n;

    if (!path || !buf || cap == 0) {
        return -1;
    }
    if (vfs_open(vfs_get_drive(), path, VFS_O_READ, &f) != 0) {
        return -1;
    }
    n = cap > 0xFFFFFFFFu ? 0xFFFFFFFEu : (uint32_t)cap - 1;
    if (vfs_read(&f, buf, n, &got) != 0) {
        vfs_close(&f);
        return -1;
    }
    buf[got] = 0;
    if (out_len) {
        *out_len = got;
    }
    vfs_close(&f);
    return 0;
}

static int api_stat_file(const char *path, uint32_t *size, int *is_dir) {
    if (!path) {
        return -1;
    }
    return vfs_stat(vfs_get_drive(), path, size, is_dir);
}

static int api_read_at(const char *path, uint32_t offset, void *buf, uint32_t cap, uint32_t *out_len) {
    vfs_file_t f;
    uint32_t got = 0;

    if (!path || !buf || !out_len) {
        return -1;
    }
    *out_len = 0;
    if (vfs_open(vfs_get_drive(), path, VFS_O_READ, &f) != 0) {
        return -1;
    }
    if (vfs_seek(&f, offset) != 0 || vfs_read(&f, buf, cap, &got) != 0) {
        vfs_close(&f);
        return -1;
    }
    *out_len = got;
    vfs_close(&f);
    return 0;
}

static int api_write_file(const char *path, const char *buf, size_t len) {
    vfs_file_t f;
    uint32_t got = 0;
    uint32_t n;

    if (!path || (len > 0 && !buf)) {
        return -1;
    }
    if (vfs_open(vfs_get_drive(), path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f) != 0) {
        return -1;
    }
    n = len > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)len;
    if (n > 0 && (vfs_write(&f, buf, n, &got) != 0 || got != n)) {
        vfs_close(&f);
        return -1;
    }
    return vfs_close(&f);
}

#define FOS_FD_MAX 16

static vfs_file_t api_files[FOS_FD_MAX];
static uint16_t api_fd_owner[FOS_FD_MAX];

static vfs_file_t *api_fd(int fd) {
    if (fd < 0 || fd >= FOS_FD_MAX || !api_files[fd].used) {
        return 0;
    }
    return &api_files[fd];
}

static int api_fopen(const char *path, int flags) {
    int fd;

    if (!path || !path[0]) {
        return -1;
    }
    for (fd = 0; fd < FOS_FD_MAX; fd++) {
        if (!api_files[fd].used) {
            break;
        }
    }
    if (fd >= FOS_FD_MAX) {
        return -1;
    }
    if (vfs_open(vfs_get_drive(), path, flags, &api_files[fd]) != 0) {
        return -1;
    }
    api_fd_owner[fd] = heap_get_owner();
    return fd;
}

static int api_fread(int fd, void *buf, uint32_t cap, uint32_t *out_len) {
    vfs_file_t *f = api_fd(fd);
    if (!f) {
        return -1;
    }
    return vfs_read(f, buf, cap, out_len);
}

static int api_fwrite(int fd, const void *data, uint32_t len, uint32_t *out_len) {
    vfs_file_t *f = api_fd(fd);
    if (!f) {
        return -1;
    }
    return vfs_write(f, data, len, out_len);
}

static int api_fseek(int fd, uint32_t offset) {
    vfs_file_t *f = api_fd(fd);
    if (!f) {
        return -1;
    }
    return vfs_seek(f, offset);
}

static int api_ftell(int fd, uint32_t *offset) {
    vfs_file_t *f = api_fd(fd);
    if (!f || !offset) {
        return -1;
    }
    *offset = vfs_tell(f);
    return 0;
}

static int api_fsize(int fd, uint32_t *size) {
    vfs_file_t *f = api_fd(fd);
    if (!f || !size) {
        return -1;
    }
    *size = vfs_file_size(f);
    return 0;
}

static int api_fclose(int fd) {
    vfs_file_t *f = api_fd(fd);
    if (!f) {
        return -1;
    }
    api_fd_owner[fd] = 0;
    return vfs_close(f);
}

void fos_api_close_owner(uint16_t owner) {
    int fd;
    for (fd = 0; fd < FOS_FD_MAX; fd++) {
        if (api_files[fd].used && api_fd_owner[fd] == owner) {
            vfs_close(&api_files[fd]);
            api_fd_owner[fd] = 0;
        }
    }
}

static int api_read_dir(const char *path, fos_dir_t *out) {
    vfs_dir_list_t list;
    if (!out) {
        return -1;
    }
    out->count = 0;
    if (vfs_read_dir(vfs_get_drive(), path, &list) != 0) {
        return -1;
    }
    out->count = list.count;
    for (int i = 0; i < list.count; i++) {
        strncpy(out->entries[i].name, list.entries[i].name, 63);
        out->entries[i].name[63] = 0;
        out->entries[i].is_dir = list.entries[i].is_dir;
        out->entries[i].size = list.entries[i].size;
    }
    return 0;
}

static int api_set_cwd(const char *path) {
    return vfs_set_cwd(path);
}

static void api_get_cwd(char *buf, size_t cap) {
    if (!buf || cap == 0) {
        return;
    }
    const char *cwd = vfs_get_cwd();
    strncpy(buf, cwd ? cwd : "\\", cap - 1);
    buf[cap - 1] = 0;
}

static int api_mkdir(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    return vfs_mkdir(vfs_get_drive(), path);
}

static int api_delete_file(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    return vfs_delete(vfs_get_drive(), path);
}

static int api_copy_file(const char *src, const char *dst) {
    if (!src || !dst || !src[0] || !dst[0]) {
        return -1;
    }
    return vfs_copy(vfs_get_drive(), src, dst);
}

static int api_move_file(const char *src, const char *dst) {
    if (!src || !dst || !src[0] || !dst[0]) {
        return -1;
    }
    return vfs_move(vfs_get_drive(), src, dst);
}

static int api_get_mem_info(fos_mem_info_t *out) {
    return memory_get_info(out);
}

static int api_get_heap_info(fos_heap_info_t *out) {
    if (!out) {
        return -1;
    }
    memory_pages_stats(&out->pool_total, &out->pool_used);
    heap_stats(&out->heap_reserved, &out->heap_used, &out->heap_blocks);
    return 0;
}

static int api_run_com(const char *path, const char *args) {
    if (!path) {
        return -1;
    }
    console_begin_direct();
    int r = exec_run(vfs_get_drive(), path, args ? args : "");
    console_end_direct();
    return r;
}

static int api_run_bat(const char *path, const char *args) {
    if (!path) {
        return -1;
    }
    return shell_run_bat(path, args ? args : "");
}

static int api_mouse_poll(fos_mouse_t *out) {
    return mouse_get((mouse_state_t *)out);
}

void fos_api_init(void) {
    memset((void *)FOS_API_ADDR, 0, sizeof(fos_api_t));
    api->magic = FOS_API_MAGIC;
    api->write = console_write;
    api->write_n = console_write_n;
    api->write_line = console_write_line;
    api->putchar = console_putchar;
    api->has_key = keyboard_has_key;
    api->read_key = api_read_key;
    api->read_file = api_read_file;
    api->write_file = api_write_file;
    api->clear_screen = console_clear;
    api->get_drive = vfs_get_drive;
    api->goto_xy = console_goto_xy;
    api->read_dir = api_read_dir;
    api->set_cwd = api_set_cwd;
    api->get_cwd = api_get_cwd;
    api->run_com = api_run_com;
    api->set_color = console_set_color;
    api->write_color = console_write_color;
    api->get_ticks_ms = timer_ticks_ms;
    api->sleep_ms = timer_sleep_ms;
    api->rtc_read = rtc_read;
    api->rtc_write = rtc_write;
    api->mkdir = api_mkdir;
    api->delete_file = api_delete_file;
    api->copy_file = api_copy_file;
    api->move_file = api_move_file;
    api->get_mem_info = api_get_mem_info;
    api->irq_register = irq_register;
    api->irq_unregister = irq_unregister;
    api->irq_enable = irq_enable;
    api->irq_disable = irq_disable;
    api->irq_pending = irq_get_pending;
    api->irq_clear = irq_clear_pending;
    api->irq_in_handler = irq_in_handler;
    api->get_term_size = console_get_size;
    api->mem_alloc = heap_alloc;
    api->mem_free = heap_free;
    api->mem_realloc = heap_realloc;
    api->get_heap_info = api_get_heap_info;
    api->sound_present = sb16_present;
    api->sound_beep = sb16_beep;
    api->sound_play = sb16_play;
    api->sound_stop = sb16_stop;
    api->sound_playing = sb16_playing;
    api->stat_file = api_stat_file;
    api->read_at = api_read_at;
    api->begin_direct = console_begin_direct;
    api->end_direct = console_end_direct;
    api->sound_start = sb16_start;
    api->sound_can_queue = sb16_can_queue;
    api->show_error = console_error;
    api->set_cursor_visible = console_set_cursor_visible;
    api->set_drive = vfs_set_drive;
    api->drive_count = vfs_drive_count;
    api->getenv = env_get;
    api->setenv = env_set;
    api->begin_capture = console_begin_capture;
    api->end_capture = console_end_capture;
    api->run_bat = api_run_bat;
    api->sound_buf_size = sb16_buf_size;
    api->mouse_poll = api_mouse_poll;
    api->hit_clear = console_hit_clear;
    api->hit_add = console_hit_add;
    api->fopen = api_fopen;
    api->fread = api_fread;
    api->fwrite = api_fwrite;
    api->fseek = api_fseek;
    api->ftell = api_ftell;
    api->fsize = api_fsize;
    api->fclose = api_fclose;
    api->clip_set = console_clip_set;
    api->clip_get = console_clip_get;
    api->cmdline[0] = 0;
    api->pipe_in_len = 0;
}

void fos_api_set_cmdline(const char *args) {
    if (!args) {
        api->cmdline[0] = 0;
        return;
    }
    strncpy(api->cmdline, args, sizeof(api->cmdline) - 1);
    api->cmdline[sizeof(api->cmdline) - 1] = 0;
}

void fos_api_set_pipe(const char *data, size_t len) {
    if (!data || len == 0) {
        api->pipe_in_len = 0;
        return;
    }
    if (len >= sizeof(api->pipe_in)) {
        len = sizeof(api->pipe_in) - 1;
    }
    memcpy(api->pipe_in, data, len);
    api->pipe_in[len] = 0;
    api->pipe_in_len = len;
}

void fos_api_clear_pipe(void) {
    api->pipe_in_len = 0;
}

void fos_api_save_io(fos_api_io_t *out) {
    if (!out) {
        return;
    }
    memcpy(out->cmdline, api->cmdline, sizeof(out->cmdline));
    memcpy(out->pipe_in, api->pipe_in, sizeof(out->pipe_in));
    out->pipe_in_len = api->pipe_in_len;
}

void fos_api_restore_io(const fos_api_io_t *in) {
    if (!in) {
        return;
    }
    memcpy(api->cmdline, in->cmdline, sizeof(api->cmdline));
    memcpy(api->pipe_in, in->pipe_in, sizeof(api->pipe_in));
    api->pipe_in_len = in->pipe_in_len;
}
