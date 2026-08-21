/*
 * tsf_fos.c — TinySoundFont + TinyMidiLoader, freestanding for PLAY.COM.
 *
 * tsf.h is MIT (Bernhard Schelling / SFZero). tml.h is zlib.
 */

#include "midi_fos.h"

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int v, size_t n);

static fos_api_t *g_api;

static void *fos_malloc(size_t n) {
    if (!g_api || !g_api->mem_alloc) {
        return 0;
    }
    return g_api->mem_alloc(n);
}

static void fos_free(void *p) {
    if (g_api && g_api->mem_free) {
        g_api->mem_free(p);
    }
}

static void *fos_realloc(void *p, size_t n) {
    if (!g_api || !g_api->mem_realloc) {
        return 0;
    }
    return g_api->mem_realloc(p, n);
}

#define LN2  0.693147180559945309417
#define LN10 2.30258509299404568402
#define PI   3.14159265358979323846

static double fos_fabs(double x) {
    return x < 0.0 ? -x : x;
}

static double fos_ldexp(double x, int n) {
    union {
        double d;
        unsigned long long i;
    } u;
    int e;
    if (x == 0.0) {
        return 0.0;
    }
    u.d = x;
    e = (int)((u.i >> 52) & 0x7ffULL);
    if (e == 0x7ff) {
        return x;
    }
    if (n > 2000) {
        n = 2000;
    }
    if (n < -2000) {
        n = -2000;
    }
    e += n;
    if (e <= 0) {
        return 0.0;
    }
    if (e >= 0x7ff) {
        u.i = (u.i & 0x8000000000000000ULL) | (0x7ffULL << 52);
        return u.d;
    }
    u.i = (u.i & 0x800fffffffffffffULL) | ((unsigned long long)e << 52);
    return u.d;
}

static double fos_sqrt(double x) {
    int i;
    double y;
    if (x <= 0.0) {
        return 0.0;
    }
    y = x;
    if (x < 1.0) {
        y = 1.0;
    }
    for (i = 0; i < 20; i++) {
        y = 0.5 * (y + x / y);
    }
    return y;
}

static double fos_exp(double x) {
    double r, s, t;
    int n, i;
    if (x > 88.0) {
        return 1.0e38;
    }
    if (x < -88.0) {
        return 0.0;
    }
    n = (int)(x / LN2);
    r = x - (double)n * LN2;
    s = 1.0;
    t = 1.0;
    for (i = 1; i < 18; i++) {
        t *= r / (double)i;
        s += t;
    }
    return fos_ldexp(s, n);
}

static double fos_log(double x) {
    union {
        double d;
        unsigned long long i;
    } u;
    int e, i;
    double m, y, y2, s, t;
    if (x <= 0.0) {
        return -1.0e3;
    }
    u.d = x;
    e = (int)((u.i >> 52) & 0x7ffULL) - 1023;
    u.i = (u.i & 0xfffffffffffffULL) | (0x3ffULL << 52);
    m = u.d;
    y = (m - 1.0) / (m + 1.0);
    y2 = y * y;
    s = y;
    t = y;
    for (i = 1; i < 16; i++) {
        t *= y2;
        s += t / (double)(2 * i + 1);
    }
    return 2.0 * s + (double)e * LN2;
}

static double fos_sin(double x) {
    double s, t, x2;
    int i, sign = 1;
    if (x != x || x > 1.0e6 || x < -1.0e6) {
        return 0.0;
    }
    while (x > PI) {
        x -= 2.0 * PI;
    }
    while (x < -PI) {
        x += 2.0 * PI;
    }
    if (x < 0.0) {
        sign = -1;
        x = -x;
    }
    if (x > PI / 2.0) {
        x = PI - x;
    }
    x2 = x * x;
    s = x;
    t = x;
    for (i = 1; i < 10; i++) {
        t *= -x2 / (double)((2 * i) * (2 * i + 1));
        s += t;
    }
    return sign < 0 ? -s : s;
}

static double fos_tan(double x) {
    double c = fos_sin(x + PI / 2.0);
    if (fos_fabs(c) < 1.0e-12) {
        c = 1.0e-12;
    }
    return fos_sin(x) / c;
}

static double fos_pow(double x, double y) {
    if (x <= 0.0) {
        return 0.0;
    }
    if (y == 0.0) {
        return 1.0;
    }
    return fos_exp(y * fos_log(x));
}

static double fos_log10(double x) {
    return fos_log(x) / LN10;
}

static float fos_powf(float x, float y) {
    return (float)fos_pow((double)x, (double)y);
}

static float fos_expf(float x) {
    return (float)fos_exp((double)x);
}

static float fos_sqrtf(float x) {
    return (float)fos_sqrt((double)x);
}

#define TSF_STATIC
#define TML_STATIC
#define TSF_NO_STDIO
#define TML_NO_STDIO
#define TSF_MALLOC  fos_malloc
#define TSF_FREE    fos_free
#define TSF_REALLOC fos_realloc
#define TSF_MEMCPY  memcpy
#define TSF_MEMSET  memset
#define TML_MALLOC  fos_malloc
#define TML_FREE    fos_free
#define TML_REALLOC fos_realloc
#define TML_MEMCPY  memcpy
#define TSF_POW     fos_pow
#define TSF_POWF    fos_powf
#define TSF_EXPF    fos_expf
#define TSF_LOG     fos_log
#define TSF_TAN     fos_tan
#define TSF_LOG10   fos_log10
#define TSF_SQRT    fos_sqrt
#define TSF_SQRTF   fos_sqrtf
#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

static tsf *g_tsf;
static tml_message *g_song;
static tml_message *g_ev;
static unsigned g_ms;
static unsigned g_len;
static int g_rate;

static void midi_apply(tml_message *m) {
    float vel;
    if (!g_tsf || !m) {
        return;
    }
    switch (m->type) {
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(g_tsf, m->channel, m->program, m->channel == 9);
        break;
    case TML_NOTE_ON:
        vel = (float)(unsigned char)m->velocity / 127.0f;
        if (m->velocity <= 0) {
            tsf_channel_note_off(g_tsf, m->channel, m->key);
        } else {
            tsf_channel_note_on(g_tsf, m->channel, m->key, vel);
        }
        break;
    case TML_NOTE_OFF:
        tsf_channel_note_off(g_tsf, m->channel, m->key);
        break;
    case TML_PITCH_BEND:
        tsf_channel_set_pitchwheel(g_tsf, m->channel, m->pitch_bend);
        break;
    case TML_CONTROL_CHANGE:
        tsf_channel_midi_control(g_tsf, m->channel, m->control, m->control_value);
        break;
    default:
        break;
    }
}

void midi_fos_init(fos_api_t *api) {
    g_api = api;
}

void midi_fos_stop(void) {
    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = 0;
    }
    if (g_song) {
        tml_free(g_song);
        g_song = 0;
    }
    g_ev = 0;
    g_ms = 0;
    g_len = 0;
}

int midi_fos_start(const void *sf2, int sf2_bytes, const void *mid, int mid_bytes,
                   int rate_hz) {
    unsigned tlen = 0;

    midi_fos_stop();
    if (!sf2 || sf2_bytes < 64 || !mid || mid_bytes < 14 || rate_hz < 4000) {
        return -1;
    }
    if (rate_hz > 44100) {
        rate_hz = 44100;
    }
    g_tsf = tsf_load_memory(sf2, sf2_bytes);
    if (!g_tsf) {
        return -1;
    }
    tsf_set_output(g_tsf, TSF_MONO, rate_hz, -6.0f);
    tsf_set_max_voices(g_tsf, 48);
    {
        int ch;
        for (ch = 0; ch < 16; ch++) {
            tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9);
        }
    }
    g_song = tml_load_memory(mid, mid_bytes);
    if (!g_song) {
        midi_fos_stop();
        return -1;
    }
    tml_get_info(g_song, 0, 0, 0, 0, &tlen);
    g_len = tlen + 800u;
    g_ev = g_song;
    g_ms = 0;
    g_rate = rate_hz;
    return 0;
}

unsigned midi_fos_length_ms(void) {
    return g_len;
}

unsigned midi_fos_time_ms(void) {
    return g_ms;
}

int midi_fos_seek_ms(unsigned ms) {
    tml_message *m;
    if (!g_tsf || !g_song) {
        return -1;
    }
    tsf_reset(g_tsf);
    tsf_set_output(g_tsf, TSF_MONO, g_rate, -6.0f);
    {
        int ch;
        for (ch = 0; ch < 16; ch++) {
            tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9);
        }
    }
    if (ms > g_len) {
        ms = g_len;
    }
    g_ev = g_song;
    g_ms = 0;
    for (m = g_song; m && m->time < ms; m = m->next) {
        if (m->type != TML_NOTE_ON && m->type != TML_NOTE_OFF) {
            midi_apply(m);
        }
        g_ev = m->next;
        g_ms = m->time;
    }
    g_ms = ms;
    return 0;
}

int midi_fos_render_u8(unsigned char *out, int samples) {
    short tmp[2048];
    int i;
    int n;
    unsigned end_ms;

    if (!g_tsf || !out || samples <= 0) {
        return 0;
    }
    if ((unsigned)g_ms >= g_len) {
        return 0;
    }
    if (samples > 2048) {
        samples = 2048;
    }
    end_ms = g_ms + (unsigned)((samples * 1000ull) / (unsigned)g_rate);
    while (g_ev && g_ev->time <= end_ms) {
        midi_apply(g_ev);
        g_ev = g_ev->next;
    }
    tsf_render_short(g_tsf, tmp, samples, 0);
    n = samples;
    for (i = 0; i < n; i++) {
        int s = tmp[i] / 256 + 128;
        if (s < 0) {
            s = 0;
        }
        if (s > 255) {
            s = 255;
        }
        out[i] = (unsigned char)s;
    }
    g_ms = end_ms;
    if (g_ms > g_len) {
        g_ms = g_len;
    }
    return n;
}
