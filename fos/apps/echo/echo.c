#include "fos_api.h"

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;

    if (api->pipe_in_len > 0) {
        api->write_n(api->pipe_in, api->pipe_in_len);
        if (api->pipe_in[api->pipe_in_len - 1] != '\n') {
            api->putchar('\n');
        }
        return;
    }

    if (api->cmdline[0]) {
        api->write_line(api->cmdline);
        return;
    }

    api->putchar('\n');
}
