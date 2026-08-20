#pragma once

#include "types.h"

void timer_init(void);
void timer_on_irq(void);
uint64_t timer_ticks_ms(void);
void timer_sleep_ms(uint32_t ms);
