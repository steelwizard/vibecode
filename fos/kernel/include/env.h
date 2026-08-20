#pragma once

#include "types.h"

#define ENV_NAME_MAX  32
#define ENV_VALUE_MAX 256

/* Seed PATH from SYSTEM.INI, plus HOME / PWD / DRIVE. Call after config_init. */
void env_init(void);

const char *env_get(const char *name);
int  env_set(const char *name, const char *value);
int  env_unset(const char *name);
void env_sync_pwd(void);
void env_print(void);

/* $NAME, ${NAME}, $(1+5) math, and %1 / %PATH% (batch). Expands in unquoted
 * text and "..."; not in '...'. */
int env_expand(const char *in, char *out, size_t cap);

int env_name_start(int c);
int env_name_char(int c);

/* %0..%9 / %* for .BAT arguments. Nested scripts save/restore this. */
typedef struct {
    char arg[10][ENV_VALUE_MAX];
    char all[ENV_VALUE_MAX];
} env_params_t;

void env_set_params(const char *arg0, const char *args);
void env_params_save(env_params_t *out);
void env_params_load(const env_params_t *in);
