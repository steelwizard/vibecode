#pragma once

#include "types.h"

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_UP,
    KEY_DOWN
} key_type_t;

typedef struct {
    key_type_t type;
    char       ch;
} key_event_t;

void keyboard_init(void);
int  keyboard_has_key(void);
char keyboard_read(void);
key_event_t keyboard_read_event(void);
