#pragma once

#include "types.h"

void config_init(int drive);
const char *config_get(const char *section, const char *key);
