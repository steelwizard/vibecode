/*
 * beep.c — Sound Blaster playback.
 *
 *   beep                 440 Hz for 200 ms
 *   beep 880             880 Hz for 200 ms
 *   beep 523 400         523 Hz for 400 ms
 *   beep FILE.WAV        8/16-bit PCM WAV (or raw 8-bit unsigned 8 kHz)
 */

#include "fos_api.h"

#define PLAY_MAX 32768

static uint8_t filebuf[PLAY_MAX];
static uint8_t pcmbuf[PLAY_MAX];

static void beep_error(fos_api_t *api, const char *msg) {
    if (api->show_error) {
        api->show_error(msg);
    } else {
        api->write_line(msg);
    }
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static uint32_t parse_u32(const char *s, const char **end) {
    uint32_t v = 0;
    s = skip_ws(s);
    while (is_digit(*s)) {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    if (end) {
        *end = s;
    }
    return v;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int play_wav_or_raw(fos_api_t *api, const uint8_t *data, size_t len) {
    uint32_t rate = 8000;
    uint16_t ch = 1;
    uint16_t bits = 8;
    const uint8_t *pcm = data;
    uint32_t pcm_len = (uint32_t)len;
    uint32_t i, o;

    if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == 'F' && data[8] == 'W' && data[9] == 'A' && data[10] == 'V' &&
        data[11] == 'E') {
        size_t off = 12;
        int got_fmt = 0;
        int got_data = 0;

        while (off + 8 <= len) {
            uint32_t cksz = rd32(data + off + 4);
            const uint8_t *chunk = data + off + 8;
            if (off + 8 + cksz > len) {
                break;
            }
            if (data[off] == 'f' && data[off + 1] == 'm' && data[off + 2] == 't') {
                if (cksz >= 16 && rd16(chunk) == 1) {
                    ch = rd16(chunk + 2);
                    rate = rd32(chunk + 4);
                    bits = rd16(chunk + 14);
                    got_fmt = 1;
                }
            } else if (data[off] == 'd' && data[off + 1] == 'a' &&
                       data[off + 2] == 't' && data[off + 3] == 'a') {
                pcm = chunk;
                pcm_len = cksz;
                got_data = 1;
            }
            off += 8 + ((cksz + 1u) & ~1u);
        }
        if (!got_fmt || !got_data) {
            beep_error(api, "BEEP: not PCM WAV");
            return -1;
        }
        if (ch < 1) {
            ch = 1;
        }
    }

    o = 0;
    if (bits == 8) {
        for (i = 0; i < pcm_len && o < PLAY_MAX; i += ch) {
            pcmbuf[o++] = pcm[i];
        }
    } else if (bits == 16) {
        for (i = 0; i + 1 < pcm_len && o < PLAY_MAX; i += (uint32_t)ch * 2u) {
            int16_t s = (int16_t)(pcm[i] | ((uint16_t)pcm[i + 1] << 8));
            pcmbuf[o++] = (uint8_t)((s / 256) + 128);
        }
    } else {
        beep_error(api, "BEEP: only 8- or 16-bit PCM");
        return -1;
    }

    if (o == 0) {
        beep_error(api, "BEEP: empty sample");
        return -1;
    }
    return api->sound_play(pcmbuf, o, rate);
}

static void usage(fos_api_t *api) {
    api->write_line("usage: beep [freq [ms]]");
    api->write_line("       beep FILE.WAV");
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    const char *arg;
    size_t n = 0;

    if (!api->sound_present || !api->sound_beep || !api->sound_play) {
        beep_error(api, "BEEP: sound API missing — rebuild the kernel");
        return;
    }
    if (!api->sound_present()) {
        beep_error(api, "BEEP: no Sound Blaster (QEMU: -device sb16)");
        return;
    }

    arg = skip_ws(api->cmdline);
    if (!arg[0]) {
        if (api->sound_beep(440, 200) != 0) {
            beep_error(api, "BEEP: playback failed");
        }
        return;
    }

    if (is_digit(arg[0])) {
        const char *rest;
        uint32_t freq = parse_u32(arg, &rest);
        uint32_t ms = 200;
        rest = skip_ws(rest);
        if (*rest) {
            if (!is_digit(*rest)) {
                usage(api);
                return;
            }
            ms = parse_u32(rest, &rest);
            rest = skip_ws(rest);
            if (*rest) {
                usage(api);
                return;
            }
        }
        if (api->sound_beep(freq, ms) != 0) {
            beep_error(api, "BEEP: playback failed");
        }
        return;
    }

    {
        int fd = api->fopen(arg, FOS_O_READ);
        uint32_t got = 0;
        if (fd < 0 || api->fread(fd, filebuf, (uint32_t)sizeof(filebuf), &got) != 0 || got == 0) {
            if (fd >= 0) {
                api->fclose(fd);
            }
            beep_error(api, "BEEP: cannot read file");
            return;
        }
        api->fclose(fd);
        n = got;
    }
    if (play_wav_or_raw(api, filebuf, n) != 0) {
        beep_error(api, "BEEP: playback failed");
    }
}
