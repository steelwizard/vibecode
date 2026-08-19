/*
 * config.c — Minimal INI reader for \SYSTEM.INI on the boot volume.
 *
 * Supported syntax:
 *   ; or #  comment lines
 *   [section]
 *   key=value
 *
 * Sections and keys are matched case-insensitively. Only ASCII is supported;
 * keep values plain (e.g. layout=de, not Unicode).
 */

#include "config.h"
#include "vfs.h"
#include "string.h"

#define CONFIG_MAX 512
#define VALUE_MAX  64

/* Raw file contents loaded once by config_init(). */
static char config_buf[CONFIG_MAX];
static int config_loaded;

static int streq_ci(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

static int is_comment_line(const char *line) {
    return line[0] == ';' || line[0] == '#';
}

static int parse_section(const char *line, char *section, size_t section_sz) {
    const char *start = line;
    const char *end;

    if (*start != '[') {
        return 0;
    }
    start++;
    end = start;
    while (*end && *end != ']') {
        end++;
    }
    if (*end != ']') {
        return 0;
    }
    size_t n = (size_t)(end - start);
    if (n + 1 > section_sz) {
        n = section_sz - 1;
    }
    memcpy(section, start, n);
    section[n] = 0;
    return 1;
}

static int parse_key_value(const char *line, char *key, size_t key_sz, char *val, size_t val_sz) {
    const char *eq = line;
    size_t klen;

    while (*eq && *eq != '=') {
        eq++;
    }
    if (*eq != '=') {
        return 0;
    }

    klen = (size_t)(eq - line);
    while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t')) {
        klen--;
    }
    if (klen + 1 > key_sz) {
        klen = key_sz - 1;
    }
    memcpy(key, line, klen);
    key[klen] = 0;

    eq = skip_spaces(eq + 1);
    strncpy(val, eq, val_sz - 1);
    val[val_sz - 1] = 0;

    klen = strlen(val);
    while (klen > 0 && (val[klen - 1] == ' ' || val[klen - 1] == '\t' ||
                          val[klen - 1] == '\r')) {
        val[--klen] = 0;
    }
    return 1;
}

void config_init(int drive) {
    size_t len = 0;

    config_loaded = 0;
    config_buf[0] = 0;

    /* Missing SYSTEM.INI is fine — callers fall back to defaults. */
    if (vfs_read_file(drive, "\\SYSTEM.INI", config_buf, sizeof(config_buf) - 1, &len) != 0) {
        return;
    }
    config_buf[len] = 0;
    config_loaded = 1;
}

/* Linear scan; fine for a tiny config file. Returns static buffer — copy if needed. */
const char *config_get(const char *section, const char *key) {
    static char value[VALUE_MAX];
    char current_section[32];
    char line[128];
    char parsed_key[64];
    char parsed_val[VALUE_MAX];
    size_t i = 0;

    if (!config_loaded || !section || !key) {
        return 0;
    }

    current_section[0] = 0;

    while (config_buf[i]) {
        size_t li = 0;

        while (config_buf[i] && config_buf[i] != '\n' && li + 1 < sizeof(line)) {
            line[li++] = config_buf[i++];
        }
        if (config_buf[i] == '\n') {
            i++;
        }
        line[li] = 0;
        if (li > 0 && line[li - 1] == '\r') {
            line[li - 1] = 0;
        }

        if (line[0] == 0 || is_comment_line(line)) {
            continue;
        }

        if (parse_section(line, current_section, sizeof(current_section))) {
            continue;
        }

        if (!current_section[0] || !streq_ci(current_section, section)) {
            continue;
        }

        if (!parse_key_value(line, parsed_key, sizeof(parsed_key),
                             parsed_val, sizeof(parsed_val))) {
            continue;
        }

        if (streq_ci(parsed_key, key)) {
            strcpy(value, parsed_val);
            return value;
        }
    }

    return 0;
}
