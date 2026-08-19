#include "fos_api.h"
#include "console.h"
#include "string.h"

#define api ((fos_api_t *)FOS_API_ADDR)

void fos_api_init(void) {
    memset((void *)FOS_API_ADDR, 0, sizeof(fos_api_t));
    api->magic = FOS_API_MAGIC;
    api->write = console_write;
    api->write_n = console_write_n;
    api->write_line = console_write_line;
    api->putchar = console_putchar;
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
