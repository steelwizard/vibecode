/*
 * nova.c — Space breakout for the FOS console.
 *
 *   nova
 *
 * Arrows / A D move, space launch (or fire lasers), p pause, q quit.
 * Mouse steers the paddle; click to launch. High scores: \GAMES\NOVAHS.TXT
 */

#include "fos_api.h"

#define BR_W       13
#define BR_H        8
#define BW          2
#define FX        256
#define MAX_BALL    3
#define MAX_PU      6
#define MAX_SHOT    6
#define MAX_SPARK  28
#define MAX_STAR   90
#define HS_MAX      5
#define HS_PATH    "\\GAMES\\NOVAHS.TXT"
#define LIVES0      3
#define LIVES_MAX   6
#define SR       8000
#define SFX_CAP  1600

#define ST_TITLE    0
#define ST_PLAY     1
#define ST_PAUSE    2
#define ST_OVER     3
#define ST_WIN      4
#define ST_NAME     5

#define PU_WIDE     1
#define PU_SLOW     2
#define PU_LIFE     3
#define PU_CATCH    4
#define PU_LASER    5
#define PU_MULTI    6

#define CH_TL   0xC9u
#define CH_TR   0xBBu
#define CH_BL   0xC8u
#define CH_BR   0xBCu
#define CH_H    0xCDu
#define CH_V    0xBAu
#define CH_BLK  0xDBu
#define CH_MED  0xB1u
#define CH_DRK  0xB0u
#define CH_DOT  0xFAu
#define CH_STAR 0xF9u
#define CH_BAL  0x04u
#define CH_HRT  0x03u

#define C_VOID     0
#define C_NEB      1
#define C_FRAME   11
#define C_FRAME_D  9
#define C_TITLE   14
#define C_MUTED    8
#define C_TEXT    15
#define C_ACC     11
#define C_HOT     13
#define C_BAR      1
#define C_BTN     14
#define C_PADDLE  11
#define C_BALL    15
#define C_FIELD    0

static const uint8_t BCOL[7] = {0, 9, 11, 10, 14, 13, 12};
static const uint8_t BDIM[7] = {0, 1, 3,  2,  6,  5,  4};
static const uint8_t BVAL[7] = {0, 40, 50, 60, 70, 80, 90};

/* 8 stages. 0 empty, 1-6 colour, S silver (2 hits), G gold (3 hits). */
static const char LVL[][BR_H][BR_W + 1] = {
    {
        "6666666666666",
        "5555555555555",
        "4444444444444",
        "3333333333333",
        "2222222222222",
        "1111111111111",
        "0000000000000",
        "0000000000000",
    },
    {
        "000000G000000",
        "00000SSS00000",
        "0000666660000",
        "0005555555000",
        "0044444444400",
        "0333333333330",
        "2222222222222",
        "1111111111111",
    },
    {
        "6S6S6S6S6S6S6",
        "S5S5S5S5S5S5S",
        "4S4S4S4S4S4S4",
        "S3S3S3S3S3S3S",
        "2S2S2S2S2S2S2",
        "S1S1S1S1S1S1S",
        "0000000000000",
        "0000000000000",
    },
    {
        "GGGGGGGGGGGGG",
        "G00000000000G",
        "G0SSSSSSSSS0G",
        "G0S4444444S0G",
        "G0S4333334S0G",
        "G0SSSSSSSSS0G",
        "G00000000000G",
        "GGGGGGGGGGGGG",
    },
    {
        "0066066066066",
        "0066066066066",
        "0055055055055",
        "0004044044000",
        "0333033033033",
        "0222022022022",
        "0111011011011",
        "00S00G00S00G0",
    },
    {
        "G00000000000G",
        "0S000000000S0",
        "00G6666666G00",
        "000S55555S000",
        "0000G444G0000",
        "00000S3S00000",
        "000000G000000",
        "1111111111111",
    },
    {
        "SSSSSSSSSSSSS",
        "S666S666S666S",
        "S555S555S555S",
        "S444G444G444S",
        "S333S333S333S",
        "S222S222S222S",
        "S111S111S111S",
        "SSSSSSSSSSSSS",
    },
    {
        "GGSGGSGGSGGSG",
        "G6G5G4G3G2G1G",
        "SGGSGGSGGSGGS",
        "G6G5G4G3G2G1G",
        "GGSGGSGGSGGSG",
        "G6G5G4G3G2G1G",
        "SGGSGGSGGSGGS",
        "GGGGGGGGGGGGG",
    },
};

#define NLEVELS ((int)(sizeof(LVL) / sizeof(LVL[0])))

typedef struct {
    int x;
    int y;
    int vx;
    int vy;
    int alive;
    int stuck;
    int off;
} ball_t;

typedef struct {
    int x;
    int y;
    int kind;
    int alive;
} drop_t;

typedef struct {
    int x;
    int y;
    int alive;
} shot_t;

typedef struct {
    int x;
    int y;
    int vx;
    int life;
    unsigned char ch;
    uint8_t col;
} spark_t;

typedef struct {
    int x;
    int y;
    uint8_t col;
    unsigned char ch;
} star_t;

typedef struct {
    char name[4];
    uint32_t score;
} hs_t;

static fos_api_t *api;
static int cols;
static int rows;
static int ox;
static int oy;
static int sx;
static int fw;
static int fh;
static int state;
static int quit;
static int full_draw;

static uint8_t brick[BR_H][BR_W];
static uint8_t bhp[BR_H][BR_W];
static int bricks_left;
static ball_t balls[MAX_BALL];
static drop_t drops[MAX_PU];
static shot_t shots[MAX_SHOT];
static spark_t sparks[MAX_SPARK];
static star_t stars[MAX_STAR];
static int nstar;
static int pad_x;
static int pad_w;
static int pad_y;
static int lives;
static int level;
static uint32_t score;
static uint32_t rng;
static uint64_t wide_until;
static uint64_t slow_until;
static int catch_on;
static int laser_n;
static int served;
static int btn_l;
static int btn_r;
static int btn_fire;
static int btn_pause;
static int btn_quit;
static int name_i;
static char name_buf[4];
static hs_t hs[HS_MAX];
static int hs_dirty;
static int anim;
static uint8_t sfxbuf[SFX_CAP];
static int sfx_pend;
static int campaign_won;

static const int8_t SINE[32] = {
    0, 24, 49, 71, 90, 105, 117, 124,
    127, 124, 117, 105, 90, 71, 49, 24,
    0, -24, -49, -71, -90, -105, -117, -124,
    -127, -124, -117, -105, -90, -71, -49, -24
};

static uint64_t now_ms(void) {
    return api->get_ticks_ms ? api->get_ticks_ms() : 0;
}

static uint32_t rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fffu;
}

static int slen(const char *s) {
    int n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

static int abs_i(int v) {
    return v < 0 ? -v : v;
}

static void cell(int x, int y, uint8_t fg, uint8_t bg, unsigned char ch) {
    if (x < 0 || y < 0 || x >= cols || y >= rows) {
        return;
    }
    api->goto_xy(x, y);
    api->set_color(fg, bg);
    api->putchar((char)ch);
}

static void put_str(int x, int y, uint8_t fg, uint8_t bg, const char *s) {
    while (s && *s) {
        cell(x++, y, fg, bg, (unsigned char)*s++);
    }
}

static void fill_span(int x, int y, int n, uint8_t fg, uint8_t bg, unsigned char ch) {
    int i;
    for (i = 0; i < n; i++) {
        cell(x + i, y, fg, bg, ch);
    }
}

static void fill_rect(int x, int y, int w, int h, uint8_t fg, uint8_t bg, unsigned char ch) {
    int r;
    for (r = 0; r < h; r++) {
        fill_span(x, y + r, w, fg, bg, ch);
    }
}

static void put_u32(int x, int y, uint8_t fg, uint8_t bg, uint32_t v, int width) {
    char buf[12];
    int n = 0;
    int i;
    uint32_t t = v;

    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[12];
        int k = 0;
        while (t) {
            tmp[k++] = (char)('0' + (t % 10u));
            t /= 10u;
        }
        while (k) {
            buf[n++] = tmp[--k];
        }
    }
    if (width > n) {
        fill_span(x, y, width - n, fg, bg, ' ');
        x += width - n;
    }
    for (i = 0; i < n; i++) {
        cell(x + i, y, fg, bg, (unsigned char)buf[i]);
    }
}

static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/* ---- spacey PCM one-shots (short; sound_play waits for DMA) ---- */

static int8_t sine_at(uint32_t ph) {
    return SINE[(ph >> 11) & 31u];
}

static void play_pcm(uint32_t n) {
    if (n < 8 || n > SFX_CAP) {
        return;
    }
    if (api->sound_present && api->sound_present() && api->sound_play) {
        api->sound_play(sfxbuf, n, SR);
    }
}

static uint32_t synth_sweep(int f0, int f1, int ms, int vol, int noise) {
    uint32_t n = (uint32_t)(SR * ms) / 1000u;
    uint32_t i;
    uint32_t ph = 0;
    int att;

    if (n > SFX_CAP) {
        n = SFX_CAP;
    }
    if (n < 8) {
        n = 8;
    }
    att = (int)n / 10;
    if (att < 4) {
        att = 4;
    }
    for (i = 0; i < n; i++) {
        int f = f0 + (int)((int32_t)(f1 - f0) * (int32_t)i / (int32_t)n);
        int env;
        int s;
        uint32_t step;
        if (f < 40) {
            f = 40;
        }
        if (f > 3800) {
            f = 3800;
        }
        step = (uint32_t)f * 65536u / (uint32_t)SR;
        ph += step;
        if ((int)i < att) {
            env = vol * (int)i / att;
        } else {
            env = vol * (int)(n - i) / (int)(n - (uint32_t)att);
        }
        s = ((int)sine_at(ph) * env) / 128;
        if (noise) {
            s += ((int)(rnd() % 97u) - 48) * env / 160;
        }
        s += 128;
        if (s < 1) {
            s = 1;
        }
        if (s > 254) {
            s = 254;
        }
        sfxbuf[i] = (uint8_t)s;
    }
    return n;
}

static uint32_t synth_arp(const int *hz, int notes, int ms_each, int vol) {
    uint32_t o = 0;
    int k;
    for (k = 0; k < notes; k++) {
        uint32_t n = (uint32_t)(SR * ms_each) / 1000u;
        uint32_t i;
        uint32_t ph = 0;
        uint32_t step = (uint32_t)hz[k] * 65536u / (uint32_t)SR;
        if (o + n > SFX_CAP) {
            n = SFX_CAP - o;
        }
        for (i = 0; i < n; i++) {
            int env = vol * (int)(n - i) / (int)n;
            int s;
            ph += step;
            s = ((int)sine_at(ph) * env) / 128 + 128;
            if (s < 1) {
                s = 1;
            }
            if (s > 254) {
                s = 254;
            }
            sfxbuf[o++] = (uint8_t)s;
        }
    }
    return o;
}

enum {
    SFX_WALL = 1,
    SFX_PADDLE,
    SFX_BRICK,
    SFX_METAL,
    SFX_BREAK,
    SFX_LASER,
    SFX_POWER,
    SFX_LAUNCH,
    SFX_LIFE,
    SFX_DIE,
    SFX_WIN,
    SFX_OVER
};

static void sfx(int kind) {
    if (kind > sfx_pend) {
        sfx_pend = kind;
    }
}

static void sfx_flush(void) {
    uint32_t n = 0;
    int arp[4];
    int kind = sfx_pend;

    sfx_pend = 0;
    if (kind <= 0) {
        return;
    }

    switch (kind) {
    case SFX_WALL:
        n = synth_sweep(180, 90, 35, 70, 1);
        break;
    case SFX_PADDLE:
        n = synth_sweep(420, 280, 45, 90, 0);
        break;
    case SFX_BRICK:
        n = synth_sweep(980, 520, 50, 100, 0);
        break;
    case SFX_METAL:
        n = synth_sweep(1480, 900, 40, 110, 0);
        break;
    case SFX_BREAK:
        n = synth_sweep(320, 70, 90, 120, 1);
        break;
    case SFX_LASER:
        n = synth_sweep(2100, 700, 55, 90, 0);
        break;
    case SFX_POWER:
        arp[0] = 523;
        arp[1] = 659;
        arp[2] = 784;
        n = synth_arp(arp, 3, 40, 100);
        break;
    case SFX_LAUNCH:
        n = synth_sweep(160, 1100, 80, 95, 0);
        break;
    case SFX_LIFE:
        arp[0] = 392;
        arp[1] = 523;
        arp[2] = 659;
        arp[3] = 784;
        n = synth_arp(arp, 4, 45, 105);
        break;
    case SFX_DIE:
        n = synth_sweep(440, 80, 160, 120, 1);
        break;
    case SFX_WIN:
        arp[0] = 523;
        arp[1] = 659;
        arp[2] = 784;
        arp[3] = 1046;
        n = synth_arp(arp, 4, 70, 110);
        break;
    case SFX_OVER:
        arp[0] = 392;
        arp[1] = 330;
        arp[2] = 262;
        arp[3] = 196;
        n = synth_arp(arp, 4, 90, 110);
        break;
    default:
        return;
    }
    play_pcm(n);
}

static void hs_clear(void) {
    int i;
    for (i = 0; i < HS_MAX; i++) {
        hs[i].name[0] = '-';
        hs[i].name[1] = '-';
        hs[i].name[2] = '-';
        hs[i].name[3] = 0;
        hs[i].score = 0;
    }
}

static void load_hs(void) {
    char buf[256];
    uint32_t n = 0;
    int fd;
    int i;
    const char *p;

    hs_clear();
    hs_dirty = 0;
    if (!api->fopen) {
        return;
    }
    fd = api->fopen(HS_PATH, FOS_O_READ);
    if (fd < 0) {
        return;
    }
    if (api->fread(fd, buf, (uint32_t)sizeof(buf) - 1, &n) != 0) {
        api->fclose(fd);
        return;
    }
    api->fclose(fd);
    buf[n] = 0;
    p = buf;
    i = 0;
    while (i < HS_MAX && *p) {
        while (*p == '\r' || *p == '\n' || *p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (p[0] && p[1] && p[2] && p[3] == ' ') {
            hs[i].name[0] = p[0];
            hs[i].name[1] = p[1];
            hs[i].name[2] = p[2];
            hs[i].name[3] = 0;
            p += 4;
            hs[i].score = parse_u32(p);
            i++;
            while (*p && *p != '\n') {
                p++;
            }
        } else {
            break;
        }
    }
}

static void save_hs(void) {
    char buf[160];
    int n = 0;
    int i;
    int fd;
    uint32_t put = 0;

    if (!hs_dirty || !api->fopen) {
        return;
    }
    for (i = 0; i < HS_MAX; i++) {
        uint32_t t = hs[i].score;
        char tmp[12];
        int k = 0;
        buf[n++] = hs[i].name[0] ? hs[i].name[0] : '-';
        buf[n++] = hs[i].name[1] ? hs[i].name[1] : '-';
        buf[n++] = hs[i].name[2] ? hs[i].name[2] : '-';
        buf[n++] = ' ';
        if (t == 0) {
            buf[n++] = '0';
        } else {
            while (t) {
                tmp[k++] = (char)('0' + (t % 10u));
                t /= 10u;
            }
            while (k) {
                buf[n++] = tmp[--k];
            }
        }
        buf[n++] = '\r';
        buf[n++] = '\n';
    }
    fd = api->fopen(HS_PATH, FOS_O_WRITE | FOS_O_CREATE | FOS_O_TRUNC);
    if (fd >= 0 && api->fwrite(fd, buf, (uint32_t)n, &put) == 0 && put == (uint32_t)n &&
        api->fclose(fd) == 0) {
        hs_dirty = 0;
    } else if (fd >= 0) {
        api->fclose(fd);
    }
}

static int hs_qualifies(uint32_t sc) {
    int i;
    if (sc == 0) {
        return 0;
    }
    for (i = 0; i < HS_MAX; i++) {
        if (sc > hs[i].score) {
            return 1;
        }
    }
    return 0;
}

static void hs_insert(const char *name, uint32_t sc) {
    int i;
    int j;
    for (i = 0; i < HS_MAX; i++) {
        if (sc > hs[i].score) {
            for (j = HS_MAX - 1; j > i; j--) {
                hs[j] = hs[j - 1];
            }
            hs[i].name[0] = name[0];
            hs[i].name[1] = name[1];
            hs[i].name[2] = name[2];
            hs[i].name[3] = 0;
            hs[i].score = sc;
            hs_dirty = 1;
            save_hs();
            return;
        }
    }
}

static uint32_t hi_score(void) {
    return hs[0].score;
}

static int ball_speed(void) {
    int sp = 48 + (level - 1) * 7;
    if (now_ms() < slow_until) {
        sp = sp * 3 / 5;
    }
    if (sp < 32) {
        sp = 32;
    }
    if (sp > 120) {
        sp = 120;
    }
    return sp;
}

static void layout(void) {
    int need;

    cols = 80;
    rows = 25;
    if (api->get_term_size) {
        api->get_term_size(&cols, &rows);
    }
    if (cols < 48) {
        cols = 48;
    }
    if (rows < 22) {
        rows = 22;
    }
    fw = BR_W * BW;
    fh = rows - 5;
    if (fh > 30) {
        fh = 30;
    }
    if (fh < 18) {
        fh = 18;
    }
    need = fw + 2 + 22;
    ox = (cols - need) / 2;
    if (ox < 1) {
        ox = 1;
    }
    oy = 1;
    if (rows - (fh + 3) > 2) {
        oy = (rows - (fh + 3)) / 2;
        if (oy < 1) {
            oy = 1;
        }
    }
    sx = ox + fw + 4;
    pad_y = fh - 1;
}

static void seed_stars(void) {
    int i;
    nstar = 48 + (int)(rnd() % 24u);
    if (nstar > MAX_STAR) {
        nstar = MAX_STAR;
    }
    for (i = 0; i < nstar; i++) {
        stars[i].x = (int)(rnd() % (uint32_t)cols);
        stars[i].y = (int)(rnd() % (uint32_t)rows);
        stars[i].col = (rnd() & 3u) ? 8 : 15;
        stars[i].ch = (rnd() & 7u) ? CH_DOT : CH_STAR;
    }
}

static int in_field_px(int x, int y) {
    return x >= ox && x < ox + fw + 2 && y >= oy && y < oy + fh + 2;
}

static int in_panel_px(int x, int y) {
    return x >= sx && x < sx + 20 && y >= oy && y < oy + 20;
}

static void twinkle(void) {
    int i;
    for (i = 0; i < nstar; i++) {
        if ((rnd() & 7u) == 0) {
            stars[i].col = stars[i].col == 8 ? 7 : 8;
        }
        if ((rnd() & 31u) == 0) {
            stars[i].ch = stars[i].ch == CH_DOT ? CH_STAR : CH_DOT;
        }
    }
}

static void box(int x, int y, int w, int h, uint8_t fg, uint8_t bg) {
    int i;
    cell(x, y, fg, bg, CH_TL);
    fill_span(x + 1, y, w - 2, fg, bg, CH_H);
    cell(x + w - 1, y, fg, bg, CH_TR);
    for (i = 1; i < h - 1; i++) {
        cell(x, y + i, fg, bg, CH_V);
        fill_span(x + 1, y + i, w - 2, fg, bg, ' ');
        cell(x + w - 1, y + i, fg, bg, CH_V);
    }
    cell(x, y + h - 1, fg, bg, CH_BL);
    fill_span(x + 1, y + h - 1, w - 2, fg, bg, CH_H);
    cell(x + w - 1, y + h - 1, fg, bg, CH_BR);
}

static void draw_stars(void) {
    int i;
    for (i = 0; i < nstar; i++) {
        int x = stars[i].x;
        int y = stars[i].y;
        if (in_field_px(x, y) || in_panel_px(x, y) || y == rows - 1) {
            continue;
        }
        cell(x, y, stars[i].col, C_VOID, stars[i].ch);
    }
}

static void add_spark(int fx, int fy, uint8_t col) {
    int i;
    for (i = 0; i < MAX_SPARK; i++) {
        if (sparks[i].life > 0) {
            continue;
        }
        sparks[i].x = ox + 1 + fx;
        sparks[i].y = oy + 1 + fy;
        sparks[i].vx = (int)(rnd() % 3u) - 1;
        sparks[i].life = 8 + (int)(rnd() % 8u);
        sparks[i].ch = (rnd() & 1u) ? '*' : '+';
        sparks[i].col = col;
        return;
    }
}

static void parse_brick(char c, uint8_t *type, uint8_t *hp) {
    if (c >= '1' && c <= '6') {
        *type = (uint8_t)(c - '0');
        *hp = 1;
    } else if (c == 'S') {
        *type = 7;
        *hp = 2;
    } else if (c == 'G') {
        *type = 8;
        *hp = 3;
    } else {
        *type = 0;
        *hp = 0;
    }
}

static void load_level(int lv) {
    int y;
    int x;
    const char (*pat)[BR_W + 1];

    if (lv < 1) {
        lv = 1;
    }
    if (lv > NLEVELS) {
        lv = NLEVELS;
    }
    pat = LVL[lv - 1];
    bricks_left = 0;
    for (y = 0; y < BR_H; y++) {
        for (x = 0; x < BR_W; x++) {
            parse_brick(pat[y][x], &brick[y][x], &bhp[y][x]);
            if (brick[y][x]) {
                bricks_left++;
            }
        }
    }
}

static void clear_fx(void) {
    int i;
    for (i = 0; i < MAX_PU; i++) {
        drops[i].alive = 0;
    }
    for (i = 0; i < MAX_SHOT; i++) {
        shots[i].alive = 0;
    }
    for (i = 0; i < MAX_SPARK; i++) {
        sparks[i].life = 0;
    }
}

static void reset_balls_serve(void) {
    int i;
    for (i = 0; i < MAX_BALL; i++) {
        balls[i].alive = 0;
        balls[i].stuck = 0;
    }
    balls[0].alive = 1;
    balls[0].stuck = 1;
    balls[0].off = (pad_w / 2) * FX;
    balls[0].vx = 0;
    balls[0].vy = 0;
    served = 0;
}

static void clamp_pad(void) {
    if (pad_x < 0) {
        pad_x = 0;
    }
    if (pad_x + pad_w > fw) {
        pad_x = fw - pad_w;
    }
}

static void apply_wide(void) {
    int mid = pad_x + pad_w / 2;
    pad_w = (now_ms() < wide_until) ? 10 : 6;
    pad_x = mid - pad_w / 2;
    clamp_pad();
}

static void spawn_drop(int fx, int fy) {
    int i;
    int k;
    static const int tab[8] = {PU_WIDE, PU_SLOW, PU_LASER, PU_CATCH,
                               PU_MULTI, PU_LIFE, PU_WIDE, PU_LASER};
    if ((rnd() % 100u) > 22u) {
        return;
    }
    k = tab[rnd() % 8u];
    for (i = 0; i < MAX_PU; i++) {
        if (drops[i].alive) {
            continue;
        }
        drops[i].alive = 1;
        drops[i].x = fx;
        drops[i].y = fy;
        drops[i].kind = k;
        return;
    }
}

static void hit_brick(int bx, int by) {
    uint8_t t;
    uint8_t col;
    int fx;
    int i;

    if (bx < 0 || bx >= BR_W || by < 0 || by >= BR_H || !brick[by][bx]) {
        return;
    }
    t = brick[by][bx];
    if (bhp[by][bx] > 1) {
        bhp[by][bx]--;
        sfx(SFX_METAL);
        return;
    }
    col = (t <= 6) ? BCOL[t] : (t == 7 ? 7 : 14);
    brick[by][bx] = 0;
    bhp[by][bx] = 0;
    bricks_left--;
    if (t >= 1 && t <= 6) {
        score += (uint32_t)BVAL[t] * (uint32_t)level;
    } else if (t == 7) {
        score += 120u * (uint32_t)level;
    } else {
        score += 200u * (uint32_t)level;
    }
    fx = bx * BW;
    for (i = 0; i < 3; i++) {
        add_spark(fx + (int)(rnd() % (uint32_t)BW), by, col);
    }
    spawn_drop(fx, by);
    if (t >= 1 && t <= 6) {
        sfx(SFX_BRICK);
    } else {
        sfx(SFX_BREAK);
    }
}

static int brick_at(int fx, int fy, int *bx, int *by) {
    if (fy < 0 || fy >= BR_H || fx < 0 || fx >= fw) {
        return 0;
    }
    *bx = fx / BW;
    *by = fy;
    return brick[*by][*bx] != 0;
}

static int paddle_hit(int fx, int fy) {
    return fy == pad_y && fx >= pad_x && fx < pad_x + pad_w;
}

static void bounce_paddle(ball_t *b, int fx) {
    int sp = ball_speed();
    int pos = fx - pad_x;
    int fifth;

    if (catch_on) {
        b->stuck = 1;
        b->off = pos * FX + FX / 2;
        b->vx = 0;
        b->vy = 0;
        catch_on = 0;
        served = 0;
        sfx(SFX_PADDLE);
        return;
    }
    fifth = pad_w <= 1 ? 2 : (pos * 5) / pad_w;
    b->vx = (fifth - 2) * (sp / 2);
    b->vy = -sp;
    if (b->vx < -sp * 3 / 4) {
        b->vx = -sp * 3 / 4;
    }
    if (b->vx > sp * 3 / 4) {
        b->vx = sp * 3 / 4;
    }
    if (b->vx == 0) {
        b->vx = (rnd() & 1u) ? 18 : -18;
    }
    if (b->vy > -sp / 2) {
        b->vy = -sp / 2;
    }
    b->y = (pad_y - 1) * FX - 1;
    sfx(SFX_PADDLE);
}

static int move_one_ball(ball_t *b) {
    int steps;
    int s;
    int ax;
    int ay;
    int a;

    if (!b->alive) {
        return 0;
    }
    if (b->stuck) {
        if (b->off < 0) {
            b->off = 0;
        }
        if (b->off > (pad_w - 1) * FX) {
            b->off = (pad_w - 1) * FX;
        }
        b->x = pad_x * FX + b->off;
        b->y = (pad_y - 1) * FX;
        if (b->x < 0) {
            b->x = 0;
        }
        if (b->x > (fw - 1) * FX) {
            b->x = (fw - 1) * FX;
        }
        return 0;
    }
    ax = abs_i(b->vx);
    ay = abs_i(b->vy);
    a = ax > ay ? ax : ay;
    steps = a > FX / 2 ? (a * 2) / FX : 1;
    if (steps < 1) {
        steps = 1;
    }
    if (steps > 8) {
        steps = 8;
    }
    for (s = 0; s < steps; s++) {
        int mx = b->vx / steps;
        int my = b->vy / steps;
        int nx;
        int ny;
        int fx;
        int fy;
        int bx;
        int by;
        if (s == steps - 1) {
            mx = b->vx - (b->vx / steps) * (steps - 1);
            my = b->vy - (b->vy / steps) * (steps - 1);
        }
        nx = b->x + mx;
        fx = nx / FX;
        fy = b->y / FX;
        if (fx < 0) {
            b->x = 0;
            b->vx = abs_i(b->vx);
            sfx(SFX_WALL);
        } else if (fx >= fw) {
            b->x = (fw - 1) * FX;
            b->vx = -abs_i(b->vx);
            sfx(SFX_WALL);
        } else if (brick_at(fx, fy, &bx, &by)) {
            hit_brick(bx, by);
            b->vx = -b->vx;
        } else {
            b->x = nx;
        }
        ny = b->y + my;
        fx = b->x / FX;
        fy = ny / FX;
        if (fy < 0) {
            b->y = 0;
            b->vy = abs_i(b->vy);
            sfx(SFX_WALL);
        } else if (fy >= fh) {
            b->alive = 0;
            return 1;
        } else if (paddle_hit(fx, fy) && b->vy > 0) {
            bounce_paddle(b, fx);
            break;
        } else if (brick_at(fx, fy, &bx, &by)) {
            hit_brick(bx, by);
            b->vy = -b->vy;
        } else {
            b->y = ny;
        }
    }
    return 0;
}

static void launch(void) {
    int i;
    int sp = ball_speed();
    int any = 0;
    for (i = 0; i < MAX_BALL; i++) {
        ball_t *b = &balls[i];
        if (!b->alive || !b->stuck) {
            continue;
        }
        b->stuck = 0;
        b->vy = -sp;
        b->vx = ((b->off / FX) - pad_w / 2) * (sp / 3);
        if (b->vx == 0) {
            b->vx = (rnd() & 1u) ? 20 : -20;
        }
        any = 1;
    }
    if (any) {
        served = 1;
        sfx(SFX_LAUNCH);
    }
}

static void fire_laser(void) {
    int i;
    int x;
    if (laser_n <= 0 || !served) {
        return;
    }
    x = pad_x + pad_w / 2;
    for (i = 0; i < MAX_SHOT; i++) {
        if (shots[i].alive) {
            continue;
        }
        shots[i].alive = 1;
        shots[i].x = x;
        shots[i].y = pad_y - 1;
        laser_n--;
        sfx(SFX_LASER);
        return;
    }
}

static void add_multi(void) {
    int src = -1;
    int i;
    int added = 0;
    int sp = ball_speed();
    for (i = 0; i < MAX_BALL; i++) {
        if (balls[i].alive && !balls[i].stuck) {
            src = i;
            break;
        }
    }
    if (src < 0) {
        return;
    }
    for (i = 0; i < MAX_BALL && added < 2; i++) {
        if (balls[i].alive) {
            continue;
        }
        balls[i] = balls[src];
        balls[i].vx = (added == 0) ? -sp / 2 : sp / 2;
        balls[i].vy = balls[src].vy;
        if (balls[i].vy > -20) {
            balls[i].vy = -sp;
        }
        added++;
    }
}

static void collect_drop(int kind) {
    score += 25;
    switch (kind) {
    case PU_WIDE:
        wide_until = now_ms() + 12000;
        apply_wide();
        sfx(SFX_POWER);
        break;
    case PU_SLOW:
        slow_until = now_ms() + 9000;
        sfx(SFX_POWER);
        break;
    case PU_LIFE:
        if (lives < LIVES_MAX) {
            lives++;
        }
        sfx(SFX_LIFE);
        break;
    case PU_CATCH:
        catch_on = 1;
        sfx(SFX_POWER);
        break;
    case PU_LASER:
        laser_n += 12;
        if (laser_n > 36) {
            laser_n = 36;
        }
        sfx(SFX_POWER);
        break;
    case PU_MULTI:
        add_multi();
        sfx(SFX_POWER);
        break;
    default:
        break;
    }
}

static void next_level(void) {
    level++;
    if (level > NLEVELS) {
        campaign_won = 1;
        sfx(SFX_WIN);
        if (hs_qualifies(score)) {
            name_buf[0] = 'A';
            name_buf[1] = 'A';
            name_buf[2] = 'A';
            name_buf[3] = 0;
            name_i = 0;
            state = ST_NAME;
        } else {
            state = ST_WIN;
        }
        return;
    }
    load_level(level);
    wide_until = 0;
    slow_until = 0;
    catch_on = 0;
    laser_n = 0;
    apply_wide();
    clear_fx();
    reset_balls_serve();
    sfx(SFX_WIN);
    full_draw = 1;
}

static void new_game(void) {
    score = 0;
    lives = LIVES0;
    level = 1;
    campaign_won = 0;
    wide_until = 0;
    slow_until = 0;
    catch_on = 0;
    laser_n = 0;
    pad_w = 6;
    pad_x = (fw - pad_w) / 2;
    load_level(level);
    clear_fx();
    reset_balls_serve();
    state = ST_PLAY;
    full_draw = 1;
}

static int alive_balls(void) {
    int n = 0;
    int i;
    for (i = 0; i < MAX_BALL; i++) {
        if (balls[i].alive) {
            n++;
        }
    }
    return n;
}

static void lose_life(void) {
    lives--;
    sfx(SFX_DIE);
    if (lives <= 0) {
        campaign_won = 0;
        if (hs_qualifies(score)) {
            name_buf[0] = 'A';
            name_buf[1] = 'A';
            name_buf[2] = 'A';
            name_buf[3] = 0;
            name_i = 0;
            state = ST_NAME;
        } else {
            state = ST_OVER;
            sfx(SFX_OVER);
        }
        return;
    }
    wide_until = 0;
    apply_wide();
    catch_on = 0;
    reset_balls_serve();
}

static void tick_sparks(void) {
    int i;
    for (i = 0; i < MAX_SPARK; i++) {
        if (sparks[i].life <= 0) {
            continue;
        }
        sparks[i].x += sparks[i].vx;
        if ((anim & 1) == 0) {
            sparks[i].y++;
        }
        sparks[i].life--;
    }
}

static void tick_drops(void) {
    int i;
    if ((anim % 3) != 0) {
        return;
    }
    for (i = 0; i < MAX_PU; i++) {
        if (!drops[i].alive) {
            continue;
        }
        drops[i].y++;
        if (drops[i].y >= fh) {
            drops[i].alive = 0;
            continue;
        }
        if (drops[i].y == pad_y && drops[i].x >= pad_x &&
            drops[i].x < pad_x + pad_w) {
            collect_drop(drops[i].kind);
            drops[i].alive = 0;
        }
    }
}

static void tick_shots(void) {
    int i;
    for (i = 0; i < MAX_SHOT; i++) {
        int bx;
        int by;
        if (!shots[i].alive) {
            continue;
        }
        shots[i].y -= 2;
        if (shots[i].y < 0) {
            shots[i].alive = 0;
            continue;
        }
        if (brick_at(shots[i].x, shots[i].y, &bx, &by) ||
            brick_at(shots[i].x, shots[i].y + 1, &bx, &by)) {
            hit_brick(bx, by);
            shots[i].alive = 0;
        }
    }
}

static void tick_play(void) {
    int i;

    apply_wide();
    for (i = 0; i < MAX_BALL; i++) {
        (void)move_one_ball(&balls[i]);
    }
    tick_drops();
    tick_shots();
    tick_sparks();
    if (bricks_left <= 0) {
        next_level();
        return;
    }
    if (alive_balls() == 0) {
        lose_life();
    }
}

static void draw_brick(int bx, int by) {
    uint8_t t = brick[by][bx];
    uint8_t hp = bhp[by][bx];
    uint8_t fg;
    uint8_t bg;
    unsigned char ch;
    int x;
    int y;
    int i;

    if (!t) {
        return;
    }
    if (t <= 6) {
        bg = BCOL[t];
        fg = BDIM[t];
        ch = CH_BLK;
    } else if (t == 7) {
        bg = 7;
        fg = 8;
        ch = hp > 1 ? CH_BLK : CH_MED;
    } else {
        bg = 14;
        fg = 6;
        ch = hp > 1 ? CH_BLK : CH_MED;
    }
    x = ox + 1 + bx * BW;
    y = oy + 1 + by;
    cell(x, y, bg, bg, ch);
    if (BW > 1) {
        cell(x + BW - 1, y, fg, fg, ch);
        for (i = 1; i < BW - 1; i++) {
            cell(x + i, y, bg, bg, ch);
        }
    }
}

static void draw_hud_panel(void) {
    int i;
    int y;

    box(sx, oy, 20, 20, C_FRAME_D, C_VOID);
    put_str(sx + 2, oy, C_HOT, C_VOID, " NOVA ");
    put_str(sx + 2, oy + 2, C_MUTED, C_VOID, "SCORE");
    put_u32(sx + 2, oy + 3, C_TITLE, C_VOID, score, 12);
    put_str(sx + 2, oy + 5, C_MUTED, C_VOID, "BEST");
    put_u32(sx + 2, oy + 6, C_ACC, C_VOID, hi_score(), 12);
    put_str(sx + 2, oy + 8, C_MUTED, C_VOID, "SECTOR");
    put_u32(sx + 10, oy + 8, C_TEXT, C_VOID, (uint32_t)level, 2);
    put_str(sx + 2, oy + 9, C_MUTED, C_VOID, "SHIPS");
    fill_span(sx + 8, oy + 9, 8, C_MUTED, C_VOID, ' ');
    for (i = 0; i < lives && i < 6; i++) {
        cell(sx + 8 + i, oy + 9, 12, C_VOID, CH_HRT);
    }

    y = oy + 11;
    put_str(sx + 2, y, C_MUTED, C_VOID, "GEAR");
    fill_span(sx + 2, y + 1, 16, C_MUTED, C_VOID, ' ');
    if (now_ms() < wide_until) {
        put_str(sx + 2, y + 1, 11, C_VOID, "WIDE");
    }
    if (now_ms() < slow_until) {
        put_str(sx + 7, y + 1, 9, C_VOID, "SLOW");
    }
    if (catch_on) {
        put_str(sx + 12, y + 1, 13, C_VOID, "LOCK");
    }
    fill_span(sx + 2, y + 2, 16, C_MUTED, C_VOID, ' ');
    if (laser_n > 0) {
        put_str(sx + 2, y + 2, 12, C_VOID, "LASER");
        put_u32(sx + 8, y + 2, C_TEXT, C_VOID, (uint32_t)laser_n, 3);
    }

    put_str(sx + 2, oy + 14, C_MUTED, C_VOID, "HALL OF FAME");
    for (i = 0; i < 3 && i < HS_MAX; i++) {
        put_str(sx + 2, oy + 15 + i, C_ACC, C_VOID, hs[i].name);
        put_u32(sx + 6, oy + 15 + i, C_TEXT, C_VOID, hs[i].score, 10);
    }
}

static void draw_play(void) {
    int x;
    int y;
    int i;
    const char *ov = 0;
    uint8_t ovbg = C_TITLE;

    if (full_draw) {
        fill_rect(0, 0, cols, rows, C_MUTED, C_VOID, ' ');
        draw_stars();
        box(ox, oy, fw + 2, fh + 2, C_FRAME, C_FIELD);
        put_str(ox + 2, oy, C_HOT, C_FIELD, " NOVA ");
        draw_hud_panel();
        fill_span(0, rows - 1, cols, C_TEXT, C_BAR, ' ');
        {
            int b = 2;
            btn_l = b;
            put_str(b, rows - 1, C_BTN, C_BAR, "< ");
            b += 3;
            btn_r = b;
            put_str(b, rows - 1, C_BTN, C_BAR, "> ");
            b += 3;
            btn_fire = b;
            put_str(b, rows - 1, C_BTN, C_BAR, "Fire ");
            b += 6;
            btn_pause = b;
            put_str(b, rows - 1, C_BTN, C_BAR, "Pause ");
            b += 7;
            btn_quit = b;
            put_str(b, rows - 1, C_HOT, C_BAR, "Quit");
        }
        full_draw = 0;
    } else {
        fill_rect(ox + 1, oy + 1, fw, fh, C_MUTED, C_FIELD, ' ');
        draw_hud_panel();
    }

    for (y = 0; y < BR_H; y++) {
        for (x = 0; x < BR_W; x++) {
            if (brick[y][x]) {
                draw_brick(x, y);
            }
        }
    }
    for (i = 0; i < MAX_SPARK; i++) {
        if (sparks[i].life > 0) {
            cell(sparks[i].x, sparks[i].y, sparks[i].col, C_FIELD, sparks[i].ch);
        }
    }
    for (i = 0; i < MAX_PU; i++) {
        if (drops[i].alive) {
            unsigned char ch = '?';
            uint8_t col = 15;
            switch (drops[i].kind) {
            case PU_WIDE:
                ch = 'W';
                col = 11;
                break;
            case PU_SLOW:
                ch = 'S';
                col = 9;
                break;
            case PU_LIFE:
                ch = 'L';
                col = 10;
                break;
            case PU_CATCH:
                ch = 'C';
                col = 13;
                break;
            case PU_LASER:
                ch = 'F';
                col = 12;
                break;
            case PU_MULTI:
                ch = 'M';
                col = 14;
                break;
            default:
                break;
            }
            cell(ox + 1 + drops[i].x, oy + 1 + drops[i].y, 0, col, ch);
        }
    }
    for (i = 0; i < MAX_SHOT; i++) {
        if (shots[i].alive) {
            cell(ox + 1 + shots[i].x, oy + 1 + shots[i].y, 12, C_FIELD, '|');
        }
    }
    fill_span(ox + 1 + pad_x, oy + 1 + pad_y, pad_w, C_PADDLE, C_PADDLE, CH_BLK);
    cell(ox + 1 + pad_x, oy + 1 + pad_y, 15, C_PADDLE, 0xDDu);
    cell(ox + 1 + pad_x + pad_w - 1, oy + 1 + pad_y, 9, C_PADDLE, 0xDEu);
    for (i = 0; i < MAX_BALL; i++) {
        if (!balls[i].alive) {
            continue;
        }
        x = ox + 1 + balls[i].x / FX;
        y = oy + 1 + balls[i].y / FX;
        cell(x, y, 14, C_FIELD, CH_BAL);
    }

    if (!served && state == ST_PLAY) {
        ov = " SPACE TO LAUNCH ";
        ovbg = 11;
    }
    if (state == ST_PAUSE) {
        ov = " PAUSED — p RESUME ";
        ovbg = C_TITLE;
    } else if (state == ST_OVER) {
        ov = " HULL BREACH — Enter retry ";
        ovbg = 12;
    } else if (state == ST_WIN) {
        ov = " SECTOR CLEAR — Enter ";
        ovbg = 10;
    } else if (state == ST_NAME) {
        ov = 0;
        put_str(ox + 3, oy + fh / 2 - 1, 0, 14, " NEW HIGH SCORE ");
        {
            int tx = ox + fw / 2 - 2;
            int k;
            for (k = 0; k < 3; k++) {
                uint8_t bg = (k == name_i) ? 13 : 1;
                cell(tx + k, oy + fh / 2 + 1, 15, bg, (unsigned char)name_buf[k]);
            }
        }
        put_str(ox + 2, oy + fh / 2 + 3, C_MUTED, C_FIELD, "type / arrows  Enter");
    }
    if (ov) {
        int n = slen(ov);
        int tx = ox + 1;
        if (n < fw) {
            tx = ox + 1 + (fw - n) / 2;
        }
        put_str(tx, oy + fh / 2, 0, ovbg, ov);
    }
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

static void draw_title(void) {
    static const char *logo[6] = {
        "#  #  ###  #   #   ## ",
        "## # #   # #   #  #  #",
        "# ## #   # #   #  ####",
        "#  # #   #  # #   #  #",
        "#  #  ###    #    #  #",
        "                      ",
    };
    static const uint8_t lc[5] = {11, 13, 9, 12, 14};
    int i;
    int j;
    int lx;
    int ly;
    int tw = 22;

    fill_rect(0, 0, cols, rows, C_MUTED, C_VOID, ' ');
    draw_stars();
    lx = (cols - tw) / 2;
    if (lx < 0) {
        lx = 0;
    }
    ly = rows / 2 - 10;
    if (ly < 1) {
        ly = 1;
    }
    for (i = 0; i < 5; i++) {
        for (j = 0; logo[i][j]; j++) {
            if (logo[i][j] == '#') {
                uint8_t c = lc[(i + (anim / 6)) % 5];
                cell(lx + j, ly + i, c, c, CH_BLK);
            }
        }
    }
    put_str((cols - 25) / 2, ly + 6, C_HOT, C_VOID, "S P A C E  B R E A K O U T");
    put_str((cols - 36) / 2, ly + 8, C_TEXT, C_VOID, "arrows / mouse  paddle     space  launch");
    put_str((cols - 36) / 2, ly + 9, C_TEXT, C_VOID, "catch W/S/L/C/F/M capsules      p pause");
    put_str((cols - 22) / 2, ly + 11, C_TITLE, C_VOID, "Enter or click to play");
    put_str((cols - 10) / 2, ly + 12, C_MUTED, C_VOID, "q to leave");

    put_str((cols - 14) / 2, ly + 14, C_ACC, C_VOID, "HALL OF FAME");
    for (i = 0; i < HS_MAX; i++) {
        int y = ly + 15 + i;
        char num[3];
        num[0] = (char)('1' + i);
        num[1] = '.';
        num[2] = 0;
        put_str((cols - 20) / 2, y, C_MUTED, C_VOID, num);
        put_str((cols - 20) / 2 + 3, y, C_TEXT, C_VOID, hs[i].name);
        put_u32((cols - 20) / 2 + 7, y, C_TITLE, C_VOID, hs[i].score, 8);
    }

    fill_span(0, rows - 1, cols, C_TEXT, C_BAR, ' ');
    put_str(2, rows - 1, C_BTN, C_BAR, "Play");
    put_str(8, rows - 1, C_HOT, C_BAR, "Quit");
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

static void move_pad(int dx) {
    pad_x += dx;
    clamp_pad();
}

static void on_quit(void) {
    save_hs();
    quit = 1;
}

static void finish_name(int save) {
    if (save) {
        hs_insert(name_buf, score);
    }
    state = campaign_won ? ST_WIN : ST_OVER;
    if (!campaign_won) {
        sfx(SFX_OVER);
    }
    full_draw = 1;
}

static void handle_name_key(fos_key_event_t ev) {
    int ch;
    if (ev.type == FOS_KEY_ENTER) {
        finish_name(1);
        return;
    }
    if (ev.type == FOS_KEY_LEFT) {
        name_i = (name_i + 2) % 3;
        return;
    }
    if (ev.type == FOS_KEY_RIGHT) {
        name_i = (name_i + 1) % 3;
        return;
    }
    if (ev.type == FOS_KEY_UP) {
        name_buf[name_i]++;
        if (name_buf[name_i] > 'Z') {
            name_buf[name_i] = 'A';
        }
        return;
    }
    if (ev.type == FOS_KEY_DOWN) {
        name_buf[name_i]--;
        if (name_buf[name_i] < 'A') {
            name_buf[name_i] = 'Z';
        }
        return;
    }
    if (ev.type != FOS_KEY_CHAR) {
        return;
    }
    ch = ev.ch;
    if (ch == 3) {
        on_quit();
        return;
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = ch - 'a' + 'A';
    }
    if (ch >= 'A' && ch <= 'Z') {
        name_buf[name_i] = (char)ch;
        if (name_i < 2) {
            name_i++;
        }
    }
}

static void handle_key(fos_key_event_t ev) {
    int ch;

    if (ev.type == FOS_KEY_NONE) {
        return;
    }
    if (ev.type == FOS_KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 3)) {
        if (state == ST_NAME) {
            finish_name(0);
            return;
        }
        on_quit();
        return;
    }
    if (state == ST_NAME) {
        handle_name_key(ev);
        return;
    }
    if (state == ST_TITLE) {
        if (ev.type != FOS_KEY_NONE) {
            new_game();
        }
        return;
    }
    if (state == ST_OVER || state == ST_WIN) {
        if (ev.type == FOS_KEY_ENTER) {
            new_game();
        }
        return;
    }
    if (state == ST_PAUSE) {
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'p' || ev.ch == 'P')) {
            state = ST_PLAY;
            full_draw = 1;
        }
        return;
    }
    if (ev.type == FOS_KEY_LEFT) {
        move_pad(-2);
        return;
    }
    if (ev.type == FOS_KEY_RIGHT) {
        move_pad(2);
        return;
    }
    if (ev.type == FOS_KEY_UP || ev.type == FOS_KEY_ENTER) {
        if (!served) {
            launch();
        } else {
            fire_laser();
        }
        return;
    }
    if (ev.type != FOS_KEY_CHAR) {
        return;
    }
    ch = ev.ch;
    if (ch == 'a' || ch == 'A') {
        move_pad(-2);
    } else if (ch == 'd' || ch == 'D') {
        move_pad(2);
    } else if (ch == ' ') {
        if (!served) {
            launch();
        } else {
            fire_laser();
        }
    } else if (ch == 'p' || ch == 'P') {
        state = ST_PAUSE;
        full_draw = 1;
    }
}

static int handle_mouse(void) {
    fos_mouse_t m;
    int mx;

    if (!api->mouse_poll || !api->mouse_poll(&m)) {
        return 0;
    }
    if (state == ST_PLAY) {
        mx = m.x - (ox + 1) - pad_w / 2;
        pad_x = mx;
        clamp_pad();
    }
    if (!(m.pending & 3)) {
        return state == ST_PLAY ? 1 : 0;
    }
    if (state == ST_TITLE) {
        if (m.y == rows - 1 && m.x >= 8 && m.x < 12) {
            on_quit();
            return 1;
        }
        new_game();
        return 1;
    }
    if (m.y == rows - 1) {
        if (m.x >= btn_quit && m.x < btn_quit + 4) {
            on_quit();
        } else if (state == ST_OVER || state == ST_WIN) {
            new_game();
        } else if (state == ST_NAME) {
            finish_name(1);
        } else if (m.x >= btn_pause && m.x < btn_pause + 5) {
            if (state == ST_PAUSE) {
                state = ST_PLAY;
                full_draw = 1;
            } else if (state == ST_PLAY) {
                state = ST_PAUSE;
                full_draw = 1;
            }
        } else if (state == ST_PLAY) {
            if (m.x >= btn_l && m.x < btn_l + 2) {
                move_pad(-2);
            } else if (m.x >= btn_r && m.x < btn_r + 2) {
                move_pad(2);
            } else if (m.x >= btn_fire && m.x < btn_fire + 4) {
                if (!served) {
                    launch();
                } else {
                    fire_laser();
                }
            }
        }
        return 1;
    }
    if (state == ST_NAME) {
        finish_name(1);
        return 1;
    }
    if ((state == ST_OVER || state == ST_WIN) && (m.pending & 1)) {
        new_game();
        return 1;
    }
    if (state == ST_PLAY && (m.pending & 1)) {
        if (!served) {
            launch();
        } else {
            fire_laser();
        }
        return 1;
    }
    return 1;
}

static void hide_cursor(void) {
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

void com_main(void) {
    api = (fos_api_t *)FOS_API_ADDR;
    quit = 0;
    hs_dirty = 0;
    sfx_pend = 0;
    campaign_won = 0;
    anim = 0;

    if (api->begin_direct) {
        api->begin_direct();
    }
    layout();
    rng = (uint32_t)now_ms() ^ 0xC0DEu;
    if (rng == 0) {
        rng = 1;
    }
    seed_stars();
    load_hs();
    state = ST_TITLE;
    if (api->clear_screen) {
        api->clear_screen();
    }
    hide_cursor();
    draw_title();

    while (!quit) {
        int dirty = 0;
        int before = state;

        if (handle_mouse()) {
            dirty = 1;
        }
        while (api->has_key && api->has_key()) {
            handle_key(api->read_key());
            dirty = 1;
        }
        if (state == ST_PLAY) {
            tick_play();
            dirty = 1;
        }
        if (state != before) {
            dirty = 1;
            if (state != ST_PLAY) {
                full_draw = 1;
            }
        }
        anim++;
        if (state == ST_TITLE && (anim % 6) == 0) {
            twinkle();
            dirty = 1;
        }
        sfx_flush();
        if (dirty) {
            if (state == ST_TITLE) {
                draw_title();
            } else {
                draw_play();
            }
        }
        if (api->sleep_ms) {
            api->sleep_ms(16);
        } else {
            __asm__ volatile("pause");
        }
    }

    save_hs();
    if (api->set_color) {
        api->set_color(15, 0);
    }
    if (api->set_cursor_visible) {
        api->set_cursor_visible(1);
    }
    if (api->clear_screen) {
        api->clear_screen();
    }
    if (api->end_direct) {
        api->end_direct();
    }
}
