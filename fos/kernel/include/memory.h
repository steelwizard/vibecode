#pragma once

#include "fos_api.h"

void memory_init(void);
void memory_set_kernel_size(uint64_t size);
int  memory_get_info(fos_mem_info_t *out);
