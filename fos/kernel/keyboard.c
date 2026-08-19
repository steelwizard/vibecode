/*
 * keyboard.c — Input from PS/2 keyboard and/or COM1 serial.
 */

#include "keyboard.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64

#define COM1_DATA   0x3F8
#define COM1_LSR    0x3FD

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static int ps2_extended;

static int serial_esc_state;

static void ps2_wait_write(void) {
    for (int i = 0; i < 500000; i++) {
        if ((inb(PS2_STATUS) & 0x02) == 0) {
            return;
        }
    }
}

static void ps2_wait_read(void) {
    for (int i = 0; i < 500000; i++) {
        if (inb(PS2_STATUS) & 0x01) {
            return;
        }
    }
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_STATUS, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

static int serial_has_byte(void) {
    return (inb(COM1_LSR) & 0x01) != 0;
}

static char serial_read_char(void) {
    return (char)inb(COM1_DATA);
}

static void ps2_init_controller(void) {
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);

    while (inb(PS2_STATUS) & 0x01) {
        (void)inb(PS2_DATA);
    }

    ps2_write_cmd(0x20);
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA);

    cfg &= ~(1 << 0);
    cfg &= ~(1 << 1);
    cfg &= ~(1 << 4);
    cfg &= ~(1 << 5);

    ps2_write_cmd(0x60);
    ps2_write_data(cfg);
    ps2_write_cmd(0xAE);

    ps2_write_data(0xFF);
    (void)ps2_read_data();
    (void)ps2_read_data();

    ps2_write_data(0xF0);
    (void)ps2_read_data();
    ps2_write_data(0x01);
    (void)ps2_read_data();

    while (inb(PS2_STATUS) & 0x01) {
        (void)inb(PS2_DATA);
    }

    ps2_extended = 0;
}

void keyboard_init(void) {
    ps2_init_controller();
    serial_esc_state = 0;
}

int keyboard_has_key(void) {
    if (serial_has_byte()) {
        return 1;
    }
    return (inb(PS2_STATUS) & 0x01) != 0;
}

static key_event_t ps2_read_event(void) {
    key_event_t ev = {KEY_NONE, 0};
    uint8_t sc = inb(PS2_DATA);

    if (sc == 0xE0) {
        ps2_extended = 1;
        return ev;
    }

    if (sc == 0xE1) {
        return ev;
    }

    if (sc & 0x80) {
        ps2_extended = 0;
        return ev;
    }

    if (ps2_extended) {
        ps2_extended = 0;
        switch (sc) {
        case 0x48:
            ev.type = KEY_UP;
            return ev;
        case 0x50:
            ev.type = KEY_DOWN;
            return ev;
        default:
            return ev;
        }
    }

    if (sc < 128) {
        char c = scancode_to_ascii[sc];
        if (c == '\t') {
            ev.type = KEY_TAB;
        } else if (c == '\n') {
            ev.type = KEY_ENTER;
        } else if (c == '\b') {
            ev.type = KEY_BACKSPACE;
        } else if (c != 0) {
            ev.type = KEY_CHAR;
            ev.ch = c;
        }
    }
    return ev;
}

static key_event_t serial_read_event(void) {
    key_event_t ev = {KEY_NONE, 0};
    char c = serial_read_char();

    if (c == '\r') {
        ev.type = KEY_ENTER;
        return ev;
    }

    if (serial_esc_state == 1) {
        serial_esc_state = 0;
        if (c == '[' || c == 'O') {
            serial_esc_state = 2;
            return ev;
        }
        if (c == 0x7F) {
            ev.type = KEY_BACKSPACE;
        } else if (c >= 32 && c <= 126) {
            ev.type = KEY_CHAR;
            ev.ch = c;
        }
        return ev;
    }

    if (serial_esc_state == 2) {
        serial_esc_state = 0;
        if (c == 'A') {
            ev.type = KEY_UP;
        } else if (c == 'B') {
            ev.type = KEY_DOWN;
        }
        return ev;
    }

    if (c == 27) {
        serial_esc_state = 1;
        return ev;
    }

    if (c == 0x7F) {
        ev.type = KEY_BACKSPACE;
        return ev;
    }

    if (c == '\t') {
        ev.type = KEY_TAB;
        return ev;
    }

    if (c == '\n') {
        ev.type = KEY_ENTER;
        return ev;
    }

    if (c >= 32 && c <= 126) {
        ev.type = KEY_CHAR;
        ev.ch = c;
    }
    return ev;
}

key_event_t keyboard_read_event(void) {
    if (serial_has_byte()) {
        return serial_read_event();
    }

    if ((inb(PS2_STATUS) & 0x01) == 0) {
        key_event_t ev = {KEY_NONE, 0};
        return ev;
    }

    return ps2_read_event();
}

char keyboard_read(void) {
    key_event_t ev = keyboard_read_event();
    switch (ev.type) {
    case KEY_CHAR:
        return ev.ch;
    case KEY_ENTER:
        return '\n';
    case KEY_BACKSPACE:
        return '\b';
    case KEY_TAB:
        return '\t';
    default:
        return 0;
    }
}
