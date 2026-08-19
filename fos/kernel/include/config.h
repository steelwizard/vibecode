#pragma once

#include "types.h"

/* Load \SYSTEM.INI from the given drive (usually 0). */
void config_init(int drive);

/* Lookup key in [section]; returns static string or NULL. */
const char *config_get(const char *section, const char *key);
