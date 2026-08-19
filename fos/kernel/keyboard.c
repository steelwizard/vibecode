/*
 * keyboard.c — Input from PS/2 keyboard and/or COM1 serial.
 *
 * PS/2 uses scancode set 2 with controller translation to set 1.
 * Layout tables map scancodes to characters (US QWERTY or DE QWERTZ).
 */

#include "keyboard.h"
#include "string.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64

#define COM1_DATA   0x3F8
#define COM1_LSR    0x3FD

/* CP437 glyphs for German letters on VGA text mode. */
#define CP_SS   '\x15'
#define CP_ae   '\x84'
#define CP_Ae   '\x8E'
#define CP_oe   '\x94'
#define CP_Oe   '\x99'
#define CP_ue   '\x81'
#define CP_Ue   '\x9A'
#define CP_sz   '\xE1'
#define CP_sup2 '\xFD'
#define CP_sup3 '\xFC'

typedef struct {
    const char *name;
    char normal[128];
    char shift[128];
    char altgr[128];
} kbd_layout_t;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static kbd_layout_t layout_us;
static kbd_layout_t layout_de;
static kbd_layout_t *current_layout;
static int ps2_extended;
static int shift_count;
static int caps_lock;
static int altgr_down;
static int serial_esc_state;

static void layout_clear(kbd_layout_t *layout, const char *name) {
    memset(layout, 0, sizeof(*layout));
    layout->name = name;
}

static void layout_set(kbd_layout_t *layout, uint8_t sc, char normal, char shifted, char altgr) {
    layout->normal[sc] = normal;
    if (shifted) {
        layout->shift[sc] = shifted;
    }
    if (altgr) {
        layout->altgr[sc] = altgr;
    }
}

static char us_shift_fallback(char c) {
    switch (c) {
    case '`': return '~';
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    default:
        if (c >= 'a' && c <= 'z') {
            return (char)(c - 32);
        }
        return c;
    }
}

static void init_layout_us(void) {
    static const char us_normal[128] = {
        0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
        '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    layout_clear(&layout_us, "us");
    memcpy(layout_us.normal, us_normal, sizeof(layout_us.normal));
}

static void init_layout_de(void) {
    layout_clear(&layout_de, "de");

    layout_set(&layout_de, 0x02, '1', '!', 0);
    layout_set(&layout_de, 0x03, '2', '"', CP_sup2);
    layout_set(&layout_de, 0x04, '3', CP_SS, CP_sup3);
    layout_set(&layout_de, 0x05, '4', '$', 0);
    layout_set(&layout_de, 0x06, '5', '%', 0);
    layout_set(&layout_de, 0x07, '6', '&', 0);
    layout_set(&layout_de, 0x08, '7', '/', '{');
    layout_set(&layout_de, 0x09, '8', '(', '[');
    layout_set(&layout_de, 0x0A, '9', ')', ']');
    layout_set(&layout_de, 0x0B, '0', '=', '}');
    layout_set(&layout_de, 0x0C, CP_sz, '?', '\\');
    layout_set(&layout_de, 0x0D, '^', '`', 0);

    layout_set(&layout_de, 0x0E, '\b', 0, 0);
    layout_set(&layout_de, 0x0F, '\t', 0, 0);
    layout_set(&layout_de, 0x1C, '\n', 0, 0);

    layout_set(&layout_de, 0x10, 'q', 0, '@');
    layout_set(&layout_de, 0x11, 'w', 0, 0);
    layout_set(&layout_de, 0x12, 'e', 0, 'E');
    layout_set(&layout_de, 0x13, 'r', 0, 0);
    layout_set(&layout_de, 0x14, 't', 0, 0);
    layout_set(&layout_de, 0x15, 'z', 0, 0);
    layout_set(&layout_de, 0x16, 'u', 0, 0);
    layout_set(&layout_de, 0x17, 'i', 0, 0);
    layout_set(&layout_de, 0x18, 'o', 0, 0);
    layout_set(&layout_de, 0x19, 'p', 0, 0);
    layout_set(&layout_de, 0x1A, CP_ue, CP_Ue, 0);
    layout_set(&layout_de, 0x1B, '+', '*', '~');

    layout_set(&layout_de, 0x1E, 'a', 0, 0);
    layout_set(&layout_de, 0x1F, 's', 0, 0);
    layout_set(&layout_de, 0x20, 'd', 0, 0);
    layout_set(&layout_de, 0x21, 'f', 0, 0);
    layout_set(&layout_de, 0x22, 'g', 0, 0);
    layout_set(&layout_de, 0x23, 'h', 0, 0);
    layout_set(&layout_de, 0x24, 'j', 0, 0);
    layout_set(&layout_de, 0x25, 'k', 0, 0);
    layout_set(&layout_de, 0x26, 'l', 0, 0);
    layout_set(&layout_de, 0x27, CP_oe, CP_Oe, 0);
    layout_set(&layout_de, 0x28, CP_ae, CP_Ae, 0);
    layout_set(&layout_de, 0x29, '#', '\'', 0);

    layout_set(&layout_de, 0x2B, '<', '>', '|');
    layout_set(&layout_de, 0x2C, 'y', 0, 0);
    layout_set(&layout_de, 0x2D, 'x', 0, 0);
    layout_set(&layout_de, 0x2E, 'c', 0, 0);
    layout_set(&layout_de, 0x2F, 'v', 0, 0);
    layout_set(&layout_de, 0x30, 'b', 0, 0);
    layout_set(&layout_de, 0x31, 'n', 0, 0);
    layout_set(&layout_de, 0x32, 'm', 0, 0);
    layout_set(&layout_de, 0x33, ',', ';', 0);
    layout_set(&layout_de, 0x34, '.', ':', 0);
    layout_set(&layout_de, 0x35, '-', '_', 0);

    layout_set(&layout_de, 0x39, ' ', 0, 0);
}

static void init_layouts(void) {
    init_layout_us();
    init_layout_de();
    current_layout = &layout_de;
}

static char map_scancode(uint8_t sc) {
    kbd_layout_t *layout = current_layout;
    char c;

    if (altgr_down && layout->altgr[sc]) {
        return layout->altgr[sc];
    }

    if (shift_count > 0) {
        if (layout->shift[sc]) {
            return layout->shift[sc];
        }
        c = layout->normal[sc];
        if (c >= 'a' && c <= 'z') {
            return (char)(c - 32);
        }
        if (current_layout == &layout_us) {
            return us_shift_fallback(c);
        }
        return c;
    }

    c = layout->normal[sc];
    if (c >= 'a' && c <= 'z' && caps_lock) {
        return (char)(c - 32);
    }
    return c;
}

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

static void ps2_flush_output(void) {
    while (inb(PS2_STATUS) & 0x01) {
        (void)inb(PS2_DATA);
    }
}

static void ps2_device_cmd(uint8_t cmd) {
    ps2_write_data(cmd);
    (void)ps2_read_data();
}

static void ps2_init_controller(void) {
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);
    ps2_flush_output();

    ps2_write_cmd(0x20);
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA);

    cfg &= ~(1 << 0);
    cfg &= ~(1 << 1);
    cfg &= ~(1 << 4);
    cfg &= ~(1 << 5);
    cfg |= (1 << 6);

    ps2_write_cmd(0x60);
    ps2_write_data(cfg);
    ps2_write_cmd(0xAE);

    ps2_device_cmd(0xFF);
    (void)ps2_read_data();

    ps2_device_cmd(0xF0);
    ps2_write_data(0x02);
    (void)ps2_read_data();

    ps2_device_cmd(0xF4);
    ps2_flush_output();

    ps2_extended = 0;
    shift_count = 0;
    caps_lock = 0;
    altgr_down = 0;
}

void keyboard_init(void) {
    init_layouts();
    ps2_init_controller();
    serial_esc_state = 0;
    while (serial_has_byte()) {
        (void)serial_read_char();
    }
}

void keyboard_set_layout(const char *name) {
    if (!name) {
        return;
    }
    if (strcmp(name, "de") == 0 || strcasecmp(name, "german") == 0) {
        current_layout = &layout_de;
        return;
    }
    if (strcmp(name, "us") == 0 || strcasecmp(name, "en") == 0 ||
        strcasecmp(name, "english") == 0) {
        current_layout = &layout_us;
    }
}

const char *keyboard_get_layout(void) {
    return current_layout ? current_layout->name : "de";
}

int keyboard_has_key(void) {
    if ((inb(PS2_STATUS) & 0x01) != 0) {
        return 1;
    }
    return serial_has_byte();
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
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) {
            if (shift_count > 0) {
                shift_count--;
            }
        } else if (ps2_extended && code == 0x38) {
            altgr_down = 0;
        }
        ps2_extended = 0;
        return ev;
    }

    if (ps2_extended) {
        ps2_extended = 0;
        if (sc == 0x38) {
            altgr_down = 1;
            return ev;
        }
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

    if (sc == 0x2A || sc == 0x36) {
        if (shift_count < 2) {
            shift_count++;
        }
        return ev;
    }

    if (sc == 0x3A) {
        caps_lock = !caps_lock;
        return ev;
    }

    if (sc < 128) {
        char c = map_scancode(sc);
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

    if (c == 0x7F || c == '\b') {
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
    if ((inb(PS2_STATUS) & 0x01) != 0) {
        return ps2_read_event();
    }

    if (serial_has_byte()) {
        return serial_read_event();
    }

    return (key_event_t){KEY_NONE, 0};
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
