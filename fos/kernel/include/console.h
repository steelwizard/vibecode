#pragma once

#include "types.h"

void console_init(void);
void console_clear(void);
void console_putchar(char c);
void console_write(const char *s);
void console_write_line(const char *s);
void console_write_n(const char *s, size_t n);
void console_begin_capture(char *buf, size_t cap);
size_t console_end_capture(void);
int  console_is_capturing(void);

void console_set_color(uint8_t fg, uint8_t bg);
void console_set_theme(uint8_t fg, uint8_t bg);
void console_write_hex64(uint64_t value);
void console_write_dec(uint64_t value);
void console_write_size(uint64_t bytes);
void console_backspace(void);
void console_write_line_color(uint8_t fg, uint8_t bg, const char *s);
void console_write_color(uint8_t fg, uint8_t bg, const char *s);
void console_clear_color(uint8_t fg, uint8_t bg);
void boot_line(const char *s);
