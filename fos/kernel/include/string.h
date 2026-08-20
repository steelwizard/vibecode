#pragma once

#include "types.h"

size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
void  *memset(void *dst, int value, size_t count);
void  *memcpy(void *dst, const void *src, size_t count);
int    memcmp(const void *a, const void *b, size_t n);
const char *skip_spaces(const char *s);
int    is_digit(char c);
int    atoi(const char *s);
void   path_normalize_slashes(char *dst, const char *src, size_t sz);
/* Collapse \.\ and \..\ in an absolute path (in place). Returns 0 or -1. */
int    path_collapse(char *path);
char  *strchr(const char *s, int c);
char  *strncat(char *dst, const char *src, size_t n);
void  *memmove(void *dst, const void *src, size_t count);
