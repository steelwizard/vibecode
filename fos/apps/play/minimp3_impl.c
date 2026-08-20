int abs(int x) {
    return x < 0 ? -x : x;
}

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include "mp3dec_fos.h"

static mp3dec_t dec;

void fos_mp3_init(void) {
    mp3dec_init(&dec);
}

int fos_mp3_decode(const uint8_t *mp3, int mp3_bytes, int16_t *pcm,
                   int *frame_bytes, int *channels, int *hz) {
    mp3dec_frame_info_t info;
    int samples;

    samples = mp3dec_decode_frame(&dec, mp3, mp3_bytes, pcm, &info);
    if (frame_bytes) {
        *frame_bytes = info.frame_bytes;
    }
    if (channels) {
        *channels = info.channels;
    }
    if (hz) {
        *hz = info.hz;
    }
    return samples;
}
