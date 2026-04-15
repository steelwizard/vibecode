#define _CRT_SECURE_NO_WARNINGS
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_PATH_BUFFER 4096
#define INITIAL_LINE_CAPACITY 256

typedef struct {
    const char* pattern;
    const char* root_path;
    bool ignore_case;
    bool line_numbers;
} GrepConfig;

static void print_usage(const char* exe_name) {
    fprintf(stderr, "Usage: %s [-i] [-n] <pattern> [path]\n", exe_name);
    fprintf(stderr, "  -i   Case-insensitive search\n");
    fprintf(stderr, "  -n   Always print line numbers\n");
    fprintf(stderr, "  pattern   Text to search for\n");
    fprintf(stderr, "  path      File or directory (default: .)\n");
}

static char to_lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

static bool line_matches(const char* line, const char* pattern, bool ignore_case) {
    if (pattern[0] == '\0') {
        return true;
    }

    size_t pattern_len = strlen(pattern);
    size_t line_len = strlen(line);
    if (pattern_len > line_len) {
        return false;
    }

    for (size_t i = 0; i <= line_len - pattern_len; ++i) {
        bool ok = true;
        for (size_t j = 0; j < pattern_len; ++j) {
            char a = line[i + j];
            char b = pattern[j];
            if (ignore_case) {
                a = to_lower_ascii(a);
                b = to_lower_ascii(b);
            }
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

static char* dup_join_path(const char* base, const char* name) {
    size_t base_len = strlen(base);
    size_t name_len = strlen(name);
    bool needs_sep = base_len > 0 && base[base_len - 1] != '\\' && base[base_len - 1] != '/';
    size_t total = base_len + (needs_sep ? 1 : 0) + name_len + 1;

    char* out = (char*)malloc(total);
    if (!out) {
        return NULL;
    }

    memcpy(out, base, base_len);
    size_t pos = base_len;
    if (needs_sep) {
        out[pos++] = '\\';
    }
    memcpy(out + pos, name, name_len);
    out[pos + name_len] = '\0';
    return out;
}

static bool read_line_dynamic(FILE* f, char** line, size_t* cap) {
    if (!*line) {
        *cap = INITIAL_LINE_CAPACITY;
        *line = (char*)malloc(*cap);
        if (!*line) {
            return false;
        }
    }

    size_t len = 0;
    int ch = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (len + 1 >= *cap) {
            size_t new_cap = (*cap) * 2;
            char* resized = (char*)realloc(*line, new_cap);
            if (!resized) {
                return false;
            }
            *line = resized;
            *cap = new_cap;
        }
        (*line)[len++] = (char)ch;
        if (ch == '\n') {
            break;
        }
    }

    if (len == 0 && ch == EOF) {
        return false;
    }
    (*line)[len] = '\0';
    return true;
}

static bool grep_file(const GrepConfig* cfg, const char* file_path, bool show_line_numbers, int* matched_files) {
    FILE* f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "wingrep: could not open '%s': %s\n", file_path, strerror(errno));
        return false;
    }

    char* line = NULL;
    size_t cap = 0;
    int line_no = 0;
    bool file_had_match = false;

    while (read_line_dynamic(f, &line, &cap)) {
        ++line_no;
        if (!line_matches(line, cfg->pattern, cfg->ignore_case)) {
            continue;
        }

        file_had_match = true;
        if (show_line_numbers || cfg->line_numbers) {
            printf("%s:%d:%s", file_path, line_no, line);
        } else {
            printf("%s:%s", file_path, line);
        }

        size_t printed_len = strlen(line);
        if (printed_len == 0 || line[printed_len - 1] != '\n') {
            printf("\n");
        }
    }

    if (file_had_match) {
        (*matched_files)++;
    }

    free(line);
    fclose(f);
    return true;
}

static bool is_dot_or_dotdot(const char* name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

static bool grep_path(const GrepConfig* cfg, const char* path, int* matched_files) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "wingrep: path not found: '%s'\n", path);
        return false;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return grep_file(cfg, path, true, matched_files);
    }

    char query[MAX_PATH_BUFFER];
    int query_len = snprintf(query, sizeof(query), "%s\\*", path);
    if (query_len < 0 || query_len >= (int)sizeof(query)) {
        fprintf(stderr, "wingrep: path too long: '%s'\n", path);
        return false;
    }

    WIN32_FIND_DATAA fd;
    HANDLE handle = FindFirstFileA(query, &fd);
    if (handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "wingrep: cannot list directory '%s'\n", path);
        return false;
    }

    bool ok = true;
    do {
        if (is_dot_or_dotdot(fd.cFileName)) {
            continue;
        }

        char* child_path = dup_join_path(path, fd.cFileName);
        if (!child_path) {
            ok = false;
            fprintf(stderr, "wingrep: out of memory\n");
            break;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!grep_path(cfg, child_path, matched_files)) {
                ok = false;
            }
        } else {
            if (!grep_file(cfg, child_path, true, matched_files)) {
                ok = false;
            }
        }

        free(child_path);
    } while (FindNextFileA(handle, &fd) != 0);

    FindClose(handle);
    return ok;
}

static bool parse_args(int argc, char** argv, GrepConfig* cfg) {
    cfg->ignore_case = false;
    cfg->line_numbers = false;
    cfg->pattern = NULL;
    cfg->root_path = ".";

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] == '-' && cfg->pattern == NULL) {
            if (strcmp(arg, "-i") == 0) {
                cfg->ignore_case = true;
                continue;
            }
            if (strcmp(arg, "-n") == 0) {
                cfg->line_numbers = true;
                continue;
            }
            if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
                return false;
            }
        }

        if (cfg->pattern == NULL) {
            cfg->pattern = arg;
        } else if (cfg->root_path && strcmp(cfg->root_path, ".") == 0) {
            cfg->root_path = arg;
        } else {
            fprintf(stderr, "wingrep: unexpected argument: %s\n", arg);
            return false;
        }
    }

    return cfg->pattern != NULL;
}

int main(int argc, char** argv) {
    GrepConfig cfg;
    if (!parse_args(argc, argv, &cfg)) {
        print_usage(argv[0]);
        return 2;
    }

    int matched_files = 0;
    bool ok = grep_path(&cfg, cfg.root_path, &matched_files);
    if (!ok) {
        return 2;
    }

    return matched_files > 0 ? 0 : 1;
}
