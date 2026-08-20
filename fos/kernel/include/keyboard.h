#pragma once

#include "types.h"

/* High-level key events consumed by the shell. */
typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_TAB,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGEUP,
    KEY_PAGEDOWN
} key_type_t;

typedef struct {
    key_type_t type;
    char       ch;
} key_event_t;

void keyboard_init(void);
int  keyboard_has_key(void);
char keyboard_read(void);
key_event_t keyboard_read_event(void);
void keyboard_inject(key_type_t type, char ch);
void keyboard_inject_str(const char *s);

/* Non-blocking: drain pending keys; return 1 if Ctrl+C (0x03) was seen. */
int  keyboard_check_ctrl_c(void);

/* Layout from SYSTEM.INI or defaults: "de" (QWERTZ) / "us" (QWERTY). */
void keyboard_set_layout(const char *name);
const char *keyboard_get_layout(void);
