/*
 * console.c — VGA text mode (0xB8000) + COM1 serial output.
 */

#include "console.h"
#include "string.h"

#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t color = 0x0F;
static char *capture_buf = 0;
static size_t capture_cap = 0;
static size_t capture_len = 0;

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_ready(void) {
    return inb(COM1 + 5) & 0x20;
}

static void serial_putchar(char c) {
    while (!serial_ready()) {
    }
    outb(COM1, (uint8_t)c);
}

static void scroll(void) {
    if (cursor_y < VGA_HEIGHT) {
        return;
    }

    for (int y = 1; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_MEM[(y - 1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];
        }
    }

    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t)(' ' | ((uint16_t)color << 8));
    }

    cursor_y = VGA_HEIGHT - 1;
}

static void putchar_at(char c, int x, int y) {
    VGA_MEM[y * VGA_WIDTH + x] = (uint16_t)(c | ((uint16_t)color << 8));
}

void console_init(void) {
    serial_init();
    color = 0x0F;
    cursor_x = 0;
    cursor_y = 0;
}

void console_clear_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
    console_clear();
}

void console_set_color(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
}

/* Change colors and repaint the whole screen (used for boot → ready transition). */
void console_set_theme(uint8_t fg, uint8_t bg) {
    color = (bg << 4) | (fg & 0x0F);
    console_clear();
}

void console_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEM[i] = (uint16_t)(' ' | ((uint16_t)color << 8));
    }
    cursor_x = 0;
    cursor_y = 0;
}

/* Erase previous character during line editing (VGA + serial terminal). */
void console_backspace(void) {
    if (cursor_x > 0) {
        cursor_x--;
        putchar_at(' ', cursor_x, cursor_y);
        serial_putchar('\b');
        serial_putchar(' ');
        serial_putchar('\b');
    }
}

void console_begin_capture(char *buf, size_t cap) {
    capture_buf = buf;
    capture_cap = cap;
    capture_len = 0;
    if (cap > 0) {
        buf[0] = 0;
    }
}

size_t console_end_capture(void) {
    size_t n = capture_len;
    if (capture_buf && capture_cap > 0) {
        if (capture_len >= capture_cap) {
            capture_len = capture_cap - 1;
        }
        capture_buf[capture_len] = 0;
    }
    capture_buf = 0;
    capture_cap = 0;
    return n;
}

int console_is_capturing(void) {
    return capture_buf != 0;
}

static void capture_putchar(char c) {
    if (!capture_buf || capture_cap == 0) {
        return;
    }
    if (capture_len + 1 < capture_cap) {
        capture_buf[capture_len++] = c;
        capture_buf[capture_len] = 0;
    }
}

void console_write_n(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        console_putchar(s[i]);
    }
}
void console_putchar(char c) {
    if (capture_buf) {
        if (c == '\b') {
            if (capture_len > 0) {
                capture_len--;
                capture_buf[capture_len] = 0;
            }
            return;
        }
        capture_putchar(c);
        return;
    }

    if (c == '\b') {
        console_backspace();
        return;
    }

    serial_putchar(c);

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        scroll();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    putchar_at(c, cursor_x, cursor_y);
    cursor_x++;
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        scroll();
    }
}

static void emit_char_colored(char c, uint8_t fg, uint8_t bg) {
    if (capture_buf) {
        capture_putchar(c);
        return;
    }

    serial_putchar(c);

    if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            putchar_at(' ', cursor_x, cursor_y);
        }
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        scroll();
        return;
    }

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    uint16_t attr = (uint16_t)(((bg & 0x0F) << 4) | (fg & 0x0F));
    VGA_MEM[cursor_y * VGA_WIDTH + cursor_x] = (uint16_t)(c | (attr << 8));
    cursor_x++;
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        scroll();
    }
}

void console_write_color(uint8_t fg, uint8_t bg, const char *s) {
    while (*s) {
        emit_char_colored(*s++, fg, bg);
    }
}

void console_write_line_color(uint8_t fg, uint8_t bg, const char *s) {
    console_write_color(fg, bg, s);
    emit_char_colored('\n', fg, bg);
}

void boot_line(const char *s) {
    console_write_line_color(15, 1, s);
}

void console_write(const char *s) {
    while (*s) {
        console_putchar(*s++);
    }
}

void console_write_line(const char *s) {
    console_write(s);
    console_putchar('\n');
}

void console_write_hex64(uint64_t value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(value >> (60 - i * 4)) & 0xF];
    }
    buf[18] = 0;
    console_write(buf);
}

void console_write_dec(uint64_t value) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    if (value == 0) {
        console_putchar('0');
        return;
    }
    while (value > 0 && i > 0) {
        buf[--i] = (char)('0' + (value % 10));
        value /= 10;
    }
    console_write(buf + i);
}

void console_write_size(uint64_t bytes) {
    const char *unit = "B";
    uint64_t whole = bytes;
    uint64_t frac = 0;

    if (bytes >= 1024ULL * 1024 * 1024) {
        unit = "GiB";
        whole = bytes / (1024ULL * 1024 * 1024);
        frac = (bytes % (1024ULL * 1024 * 1024)) * 10 / (1024ULL * 1024 * 1024);
    } else if (bytes >= 1024ULL * 1024) {
        unit = "MiB";
        whole = bytes / (1024ULL * 1024);
        frac = (bytes % (1024ULL * 1024)) * 10 / (1024ULL * 1024);
    } else if (bytes >= 1024ULL) {
        unit = "KiB";
        whole = bytes / 1024ULL;
        frac = (bytes % 1024ULL) * 10 / 1024ULL;
    }

    console_write_dec(whole);
    if (frac > 0 && whole < 100) {
        console_putchar('.');
        console_putchar((char)('0' + (char)frac));
    }
    console_putchar(' ');
    console_write(unit);
}
