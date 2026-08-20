#pragma once

#include "types.h"

void sb16_init(void);
int  sb16_present(void);
int  sb16_beep(uint32_t freq_hz, uint32_t ms);
int  sb16_play(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz);
int  sb16_start(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz);
void sb16_stop(void);
int  sb16_playing(void);
int  sb16_can_queue(void);
