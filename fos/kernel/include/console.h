#pragma once

#include "types.h"
#include "video.h"

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
void console_cursor_back(void);
void console_goto_xy(int x, int y);
void console_set_cursor_visible(int visible);
/* Toggle the block cursor on a ~1 Hz blink. Call from idle input polls. */
void console_tick_cursor(void);
void console_write_line_color(uint8_t fg, uint8_t bg, const char *s);
void console_write_color(uint8_t fg, uint8_t bg, const char *s);
void console_clear_color(uint8_t fg, uint8_t bg);
void boot_line(const char *s);

/* Scrollback and full-screen program support */
void console_begin_direct(void);
void console_end_direct(void);
void console_page_up(void);
void console_page_down(void);
void console_scroll_to_bottom(void);
int  console_at_bottom(void);

void console_init_framebuffer(const video_mode_t *mode);
int  console_is_framebuffer(void);
void console_get_size(int *cols, int *rows);
void console_get_cursor(int *x, int *y);

/* Mouse pointer / text selection / button hit-rects. */
void console_mouse_move(int x, int y);
void console_mouse_pixel(int px, int py);
void console_mouse_left(int phase, int x, int y); /* 1=down 2=drag 0=up */
void console_mouse_right(int x, int y);
int  console_hit_click(int x, int y);
void console_hit_clear(void);
void console_hit_add(int x, int y, int w, int h, int action);

/* Shared clipboard (shell mouse copy and .COM clip_set/clip_get). */
#define CONSOLE_CLIP_MAX 32768
void console_clip_set(const char *s, size_t n);
size_t console_clip_get(char *buf, size_t cap);

/* Modal error: red screen, blue box, bomb + [ OK ]. Enter/Space dismisses.
 * Restores the previous screen afterwards. While capturing (pipes/redirects)
 * this just writes the message as a line so scripts still see it. */
void console_error(const char *msg);

/* Copy/move progress: grey desktop, blue box, filenames + status bar.
 * No-op while capturing. Pair begin with end. */
void console_xfer_begin(const char *title, const char *src, const char *dst);
void console_xfer_progress(uint32_t done, uint32_t total, const char *status);
void console_xfer_end(void);
