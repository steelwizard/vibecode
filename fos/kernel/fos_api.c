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

#define api ((fos_api_t *)FOS_API_ADDR)

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
    if (!path || !buf || !out_len) {
        return -1;
    }
    return vfs_read_file(vfs_get_drive(), path, buf, cap, out_len);
}

static int api_write_file(const char *path, const char *buf, size_t len) {
    if (!path || !buf) {
        return -1;
    }
    return vfs_write_file(vfs_get_drive(), path, buf, len);
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

static int api_run_com(const char *path, const char *args) {
    if (!path) {
        return -1;
    }
    console_begin_direct();
    int r = exec_run(vfs_get_drive(), path, args ? args : "");
    console_end_direct();
    return r;
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
