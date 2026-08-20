#pragma once

#define FOS_MP3_MAX_SAMPLES 2304

void fos_mp3_init(void);
int fos_mp3_decode(const unsigned char *mp3, int mp3_bytes, short *pcm,
                   int *frame_bytes, int *channels, int *hz);
