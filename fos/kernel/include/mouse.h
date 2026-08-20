#pragma once

#include "types.h"

#define MOUSE_LEFT   1u
#define MOUSE_RIGHT  2u
#define MOUSE_MIDDLE 4u
#define MOUSE_CLICK_L 1u
#define MOUSE_CLICK_R 2u

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t buttons;
    uint8_t pending;
} mouse_state_t;

void mouse_init(void);
void mouse_poll_hw(void);
void mouse_irq_restore(void);
void mouse_on_resize(void);
int  mouse_present(void);
int  mouse_is_absolute(void);
void mouse_feed(uint8_t b);
int  mouse_get(mouse_state_t *out);
