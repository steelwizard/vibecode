#pragma once

#include "fos_api.h"

/* TinySoundFont MIDI renderer. Integer at the play.c boundary. */

void midi_fos_init(fos_api_t *api);
void midi_fos_stop(void);
int  midi_fos_start(const void *sf2, int sf2_bytes, const void *mid, int mid_bytes,
                    int rate_hz);
unsigned midi_fos_length_ms(void);
unsigned midi_fos_time_ms(void);
int  midi_fos_seek_ms(unsigned ms);
/* Render unsigned 8-bit mono. Returns samples written, 0 when finished. */
int  midi_fos_render_u8(unsigned char *out, int samples);
