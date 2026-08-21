/*
 * bench.c — FOS test bench: TUI menu, prime sieve, graphics, audio.
 *
 *   bench              interactive menu
 *   bench primes       headless CPU/prime test (for smoke)
 *
 * Arrows / 1-4 / mouse select, Enter run, q back or quit.
 */

#include "fos_api.h"

#define MAX_COLS  320
#define MAX_ROWS  90

#define C_DESK     1
#define C_DESK_FG  9
#define C_SHADOW   0
#define C_WIN      0
#define C_FG      15
#define C_FRAME   11
#define C_TITLE   14
#define C_MUTED    8
#define C_ACC     10
#define C_HOT     12
#define C_INFO     3
#define C_SEL_FG   0
#define C_SEL_BG  14
#define C_BTN_FG  15
#define C_BTN_BG   1
#define C_CARD     7
#define C_CARD_FG  0

#define CH_TL   0xC9u
#define CH_TR   0xBBu
#define CH_BL   0xC8u
#define CH_BR   0xBCu
#define CH_H    0xCDu
#define CH_V    0xBAu
#define CH_FILL 0xDBu
#define CH_MED  0xB2u
#define CH_DIM  0xB1u
#define CH_LITE 0xB0u
#define CH_DOT  0xFAu
#define CH_STAR 0x0Fu
#define CH_NOTE 0x0Eu
#define CH_PTR  0x10u
#define CH_BLOCK 0xFEu

#define IN_NONE   0
#define IN_QUIT   1
#define IN_BACK   2
#define IN_ENTER  3
#define IN_UP     4
#define IN_DOWN   5
#define IN_LEFT   6
#define IN_RIGHT  7
#define IN_SPACE  8
#define IN_DIGIT  9

#define N_MENU 5
#define N_GFX  4
#define N_SND  3

static fos_api_t *g;
static int cols = 80;
static int rows = 25;
static uint32_t rng_state = 2463534242u;

static const uint8_t SINQ[65] = {
    0, 3, 6, 9, 12, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46,
    49, 51, 54, 57, 60, 63, 65, 68, 71, 73, 76, 78, 81, 83, 85, 88,
    90, 92, 94, 96, 98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127,
    127
};

/* ---- tiny libc ---- */

static size_t slen(const char *s) {
    size_t n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

static int scmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int16_t isin(int a) {
    int q = (a >> 6) & 3;
    int i = a & 63;
    int v;
    if (q == 1 || q == 3) {
        i = 64 - i;
    }
    v = SINQ[i];
    if (q >= 2) {
        v = -v;
    }
    return (int16_t)v;
}

static int16_t icos(int a) {
    return isin(a + 64);
}

static int iabs(int v) {
    return v < 0 ? -v : v;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint64_t now_ms(void) {
    return g->get_ticks_ms ? g->get_ticks_ms() : 0;
}

static void sti(void) {
    __asm__ volatile("sti");
}

static void pause_cpu(void) {
    __asm__ volatile("pause");
}

/* ---- drawing ---- */

static void put_xy(int x, int y, uint8_t fg, uint8_t bg, unsigned char c) {
    char s[2];
    if (x < 0 || y < 0 || x >= cols || y >= rows) {
        return;
    }
    s[0] = (char)c;
    s[1] = 0;
    g->goto_xy(x, y);
    g->write_color(fg, bg, s);
}

static void fill_span(int x, int y, int n, uint8_t fg, uint8_t bg, unsigned char ch) {
    char buf[MAX_COLS + 1];
    int i;
    if (n <= 0 || y < 0 || y >= rows) {
        return;
    }
    if (x < 0) {
        n += x;
        x = 0;
    }
    if (x + n > cols) {
        n = cols - x;
    }
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        buf[i] = (char)ch;
    }
    buf[n] = 0;
    g->goto_xy(x, y);
    g->write_color(fg, bg, buf);
}

static void put_str(int x, int y, uint8_t fg, uint8_t bg, const char *s, int maxn) {
    char buf[MAX_COLS + 1];
    int i = 0;
    if (!s || maxn <= 0 || y < 0 || y >= rows) {
        return;
    }
    if (x < 0) {
        return;
    }
    while (s[i] && i < maxn && i < MAX_COLS && x + i < cols) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = 0;
    if (i > 0) {
        g->goto_xy(x, y);
        g->write_color(fg, bg, buf);
    }
}

static void fill_rect(int x, int y, int w, int h, uint8_t fg, uint8_t bg, unsigned char ch) {
    int r;
    for (r = 0; r < h; r++) {
        fill_span(x, y + r, w, fg, bg, ch);
    }
}

static void draw_box(int x, int y, int w, int h, uint8_t fg, uint8_t bg) {
    int i;
    if (w < 2 || h < 2) {
        return;
    }
    put_xy(x, y, fg, bg, CH_TL);
    fill_span(x + 1, y, w - 2, fg, bg, CH_H);
    put_xy(x + w - 1, y, fg, bg, CH_TR);
    for (i = 1; i < h - 1; i++) {
        put_xy(x, y + i, fg, bg, CH_V);
        put_xy(x + w - 1, y + i, fg, bg, CH_V);
    }
    put_xy(x, y + h - 1, fg, bg, CH_BL);
    fill_span(x + 1, y + h - 1, w - 2, fg, bg, CH_H);
    put_xy(x + w - 1, y + h - 1, fg, bg, CH_BR);
}

static void draw_window(int x, int y, int w, int h, const char *title) {
    fill_rect(x + 2, y + 1, w, h, 7, C_SHADOW, ' ');
    fill_rect(x, y, w, h, C_FG, C_WIN, ' ');
    draw_box(x, y, w, h, C_FRAME, C_WIN);
    fill_span(x + 1, y + 1, w - 2, C_TITLE, C_BTN_BG, ' ');
    if (title) {
        int tw = (int)slen(title);
        int tx = x + (w - tw) / 2;
        if (tx < x + 2) {
            tx = x + 2;
        }
        put_str(tx, y + 1, C_TITLE, C_BTN_BG, title, w - 4);
    }
}

static void desktop(void) {
    int y;
    if (g->set_color) {
        g->set_color(C_DESK_FG, C_DESK);
    }
    g->clear_screen();
    for (y = 0; y < rows; y++) {
        fill_span(0, y, cols, C_DESK_FG, C_DESK, (unsigned char)((y & 1) ? CH_DOT : ' '));
    }
}

static void hide_cursor(void) {
    if (g->set_cursor_visible) {
        g->set_cursor_visible(0);
    }
}

static void show_cursor(void) {
    if (g->set_cursor_visible) {
        g->set_cursor_visible(1);
    }
}

static int fmt_u64(char *out, uint64_t v) {
    char d[24];
    int n = 0;
    int i;
    int o = 0;
    if (v == 0) {
        out[0] = '0';
        out[1] = 0;
        return 1;
    }
    while (v && n < 20) {
        d[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        out[o++] = d[i];
        if (i > 0 && (i % 3) == 0) {
            out[o++] = ',';
        }
    }
    out[o] = 0;
    return o;
}

static void put_u64(int x, int y, uint8_t fg, uint8_t bg, uint64_t v) {
    char buf[32];
    fmt_u64(buf, v);
    put_str(x, y, fg, bg, buf, 31);
}

static void put_ms(int x, int y, uint8_t fg, uint8_t bg, uint64_t ms) {
    char buf[40];
    int n = fmt_u64(buf, ms);
    buf[n++] = ' ';
    buf[n++] = 'm';
    buf[n++] = 's';
    buf[n] = 0;
    put_str(x, y, fg, bg, buf, 39);
}

static void bar(int x, int y, int w, uint32_t num, uint32_t den, uint8_t fg, uint8_t bg) {
    uint32_t fill = 0;
    uint32_t i;
    if (w < 3) {
        return;
    }
    if (den) {
        fill = (uint32_t)(((uint64_t)num * (uint64_t)(w - 2)) / den);
        if (fill > (uint32_t)(w - 2)) {
            fill = (uint32_t)(w - 2);
        }
    }
    put_xy(x, y, C_FG, bg, '[');
    for (i = 0; i < (uint32_t)(w - 2); i++) {
        if (i < fill) {
            put_xy(x + 1 + (int)i, y, fg, C_BTN_BG, CH_FILL);
        } else {
            put_xy(x + 1 + (int)i, y, C_MUTED, bg, CH_LITE);
        }
    }
    put_xy(x + w - 1, y, C_FG, bg, ']');
}

/* ---- input ---- */

static int is_quit_char(char c) {
    return c == 'q' || c == 'Q' || c == 27 || c == 3;
}

static void drain_keys(void) {
    int n = 0;
    if (!g->has_key || !g->read_key) {
        return;
    }
    while (n < 32 && g->has_key()) {
        (void)g->read_key();
        n++;
    }
}

static int classify_key(fos_key_event_t ev, int *digit) {
    if (ev.type == FOS_KEY_NONE) {
        return IN_NONE;
    }
    if (ev.type == FOS_KEY_ENTER) {
        return IN_ENTER;
    }
    if (ev.type == FOS_KEY_UP) {
        return IN_UP;
    }
    if (ev.type == FOS_KEY_DOWN) {
        return IN_DOWN;
    }
    if (ev.type == FOS_KEY_LEFT) {
        return IN_LEFT;
    }
    if (ev.type == FOS_KEY_RIGHT) {
        return IN_RIGHT;
    }
    if (ev.type == FOS_KEY_CHAR) {
        if (is_quit_char(ev.ch)) {
            return IN_BACK;
        }
        if (ev.ch == ' ' || ev.ch == '\r') {
            return ev.ch == ' ' ? IN_SPACE : IN_ENTER;
        }
        if (ev.ch >= '1' && ev.ch <= '9') {
            if (digit) {
                *digit = ev.ch - '1';
            }
            return IN_DIGIT;
        }
        if (ev.ch == '0') {
            if (digit) {
                *digit = 9;
            }
            return IN_DIGIT;
        }
    }
    return IN_NONE;
}

static int poll_in(int *digit) {
    if (digit) {
        *digit = -1;
    }
    if (g->has_key && g->has_key()) {
        return classify_key(g->read_key(), digit);
    }
    return IN_NONE;
}

static int wait_in(void) {
    sti();
    for (;;) {
        int d;
        int k = poll_in(&d);
        if (k != IN_NONE) {
            return k;
        }
        if (g->has_key) {
            (void)g->has_key(); /* keep the pointer moving */
        }
        pause_cpu();
    }
}

static int mouse_click(int *ox, int *oy) {
    fos_mouse_t m;
    if (!g->mouse_poll || !g->mouse_poll(&m) || !(m.pending & 1)) {
        return 0;
    }
    if (ox) {
        *ox = m.x;
    }
    if (oy) {
        *oy = m.y;
    }
    return 1;
}

/* ---- layout ---- */

static int win_x, win_y, win_w, win_h;

static void layout_menu(void) {
    win_w = cols - 6;
    if (win_w > 70) {
        win_w = 70;
    }
    if (win_w < 46) {
        win_w = cols > 4 ? cols - 2 : cols;
    }
    win_h = 21;
    if (win_h > rows - 1) {
        win_h = rows - 1;
    }
    win_x = (cols - win_w) / 2;
    win_y = (rows - win_h) / 2;
    if (win_x < 0) {
        win_x = 0;
    }
    if (win_y < 0) {
        win_y = 0;
    }
}

static void layout_full(void) {
    win_x = 1;
    win_y = 0;
    win_w = cols - 2;
    win_h = rows;
    if (win_w < 20) {
        win_w = cols;
        win_x = 0;
    }
}

static void footer(int y, const char *s) {
    fill_span(win_x + 1, y, win_w - 2, C_MUTED, C_WIN, ' ');
    put_str(win_x + 2, y, C_MUTED, C_WIN, s, win_w - 4);
}

/* ---- CPU / primes ---- */

typedef struct {
    uint32_t limit;
    const char *label;
    uint32_t expect;
} sieve_case_t;

static const sieve_case_t SIEVES[] = {
    {100000u, "100k", 9592u},
    {250000u, "250k", 22036u},
    {500000u, "500k", 41538u},
    {1000000u, "1M", 78498u},
    {2000000u, "2M", 148933u}
};
#define N_SIEVE ((int)(sizeof(SIEVES) / sizeof(SIEVES[0])))

static uint32_t count_sieve(uint8_t *s, uint32_t n) {
    uint32_t i;
    uint32_t c = 0;
    for (i = 2; i <= n; i++) {
        if (s[i]) {
            c++;
        }
    }
    return c;
}

static uint32_t last_prime(uint8_t *s, uint32_t n) {
    uint32_t i = n;
    while (i >= 2) {
        if (s[i]) {
            return i;
        }
        i--;
    }
    return 0;
}

static int sieve_run(uint8_t *s, uint32_t n, uint32_t *out_count, int live) {
    uint32_t i, j;
    uint64_t last = 0;
    uint32_t root;
    for (i = 0; i <= n; i++) {
        s[i] = 1;
    }
    s[0] = 0;
    s[1] = 0;
    root = 2;
    while ((uint64_t)root * root <= (uint64_t)n) {
        root++;
    }
    root--;
    for (i = 2; i <= root; i++) {
        if (!s[i]) {
            continue;
        }
        for (j = i * i; j <= n; j += i) {
            s[j] = 0;
        }
        if (live && now_ms() - last > 40u) {
            int d;
            int k;
            last = now_ms();
            bar(win_x + 4, win_y + 8, win_w - 8, i, root, C_ACC, C_WIN);
            put_str(win_x + 4, win_y + 9, C_INFO, C_WIN, "sieving...", 20);
            k = poll_in(&d);
            if (k == IN_BACK) {
                return -1;
            }
        }
    }
    *out_count = count_sieve(s, n);
    return 0;
}

static int is_prime_u(uint32_t n) {
    uint32_t i;
    if (n < 2) {
        return 0;
    }
    if ((n & 1u) == 0) {
        return n == 2;
    }
    for (i = 3; (uint64_t)i * i <= (uint64_t)n; i += 2) {
        if (n % i == 0) {
            return 0;
        }
    }
    return 1;
}

static int trial_run(uint32_t n, uint32_t *out_count, int live) {
    uint32_t i;
    uint32_t c = 0;
    uint64_t last = 0;
    for (i = 2; i <= n; i++) {
        if (is_prime_u(i)) {
            c++;
        }
        if (live && (i & 511u) == 0 && now_ms() - last > 40u) {
            int d;
            int k;
            last = now_ms();
            bar(win_x + 4, win_y + 8, win_w - 8, i, n, C_HOT, C_WIN);
            put_str(win_x + 4, win_y + 9, C_INFO, C_WIN, "trial division...", 24);
            k = poll_in(&d);
            if (k == IN_BACK) {
                return -1;
            }
        }
    }
    *out_count = c;
    return 0;
}

static uint64_t alu_run(uint32_t ms) {
    uint64_t t0 = now_ms();
    uint64_t acc = 0x9E3779B97F4A7C15ULL;
    uint64_t n = 0;
    sti();
    while (now_ms() - t0 < (uint64_t)ms) {
        uint32_t k;
        for (k = 0; k < 256; k++) {
            acc *= 6364136223846793005ULL;
            acc += 1;
            acc ^= acc >> 17;
            n++;
        }
        if (g->has_key && g->has_key()) {
            fos_key_event_t ev = g->read_key();
            if (ev.type == FOS_KEY_CHAR && is_quit_char(ev.ch)) {
                break;
            }
        }
    }
    (void)acc;
    return n;
}

static int cpu_execute(int which, int live,
                       uint32_t *sieve_n, uint32_t *sieve_c, uint64_t *sieve_ms,
                       int *sieve_ok, uint32_t *trial_n, uint32_t *trial_c,
                       uint64_t *trial_ms, uint64_t *alu_ops, uint64_t *alu_ms) {
    const sieve_case_t *sc = &SIEVES[which];
    uint8_t *s;
    uint64_t t0;
    uint32_t trial_lim;

    *sieve_n = sc->limit;
    *sieve_ok = 0;
    *sieve_c = 0;
    *sieve_ms = 0;
    *trial_c = 0;
    *trial_ms = 0;
    *alu_ops = 0;
    *alu_ms = 200;
    trial_lim = sc->limit > 80000u ? 80000u : sc->limit;
    *trial_n = trial_lim;

    if (!g->mem_alloc) {
        return -2;
    }
    s = (uint8_t *)g->mem_alloc((size_t)sc->limit + 1u);
    if (!s) {
        return -2;
    }
    t0 = now_ms();
    if (sieve_run(s, sc->limit, sieve_c, live) != 0) {
        g->mem_free(s);
        return -1;
    }
    *sieve_ms = now_ms() - t0;
    *sieve_ok = (*sieve_c == sc->expect);
    (void)last_prime(s, sc->limit);
    g->mem_free(s);

    t0 = now_ms();
    if (trial_run(trial_lim, trial_c, live) != 0) {
        return -1;
    }
    *trial_ms = now_ms() - t0;

    t0 = now_ms();
    *alu_ops = alu_run(200);
    *alu_ms = now_ms() - t0;
    if (*alu_ms == 0) {
        *alu_ms = 1;
    }
    return 0;
}

static void cpu_print_stdout(int which) {
    uint32_t sn, sc, tn, tc;
    uint64_t sms, tms, aops, ams;
    int ok;
    char nbuf[32];
    int rc;

    g->write_line("FOS Bench — prime test");
    rc = cpu_execute(which, 0, &sn, &sc, &sms, &ok, &tn, &tc, &tms, &aops, &ams);
    if (rc == -2) {
        g->write_line("FAIL: heap API missing");
        return;
    }
    g->write("  sieve 0..");
    fmt_u64(nbuf, sn);
    g->write(nbuf);
    g->write("  primes=");
    fmt_u64(nbuf, sc);
    g->write(nbuf);
    g->write("  ");
    fmt_u64(nbuf, sms);
    g->write(nbuf);
    g->write_line(" ms");
    g->write(ok ? "  expected match: PASS" : "  expected match: FAIL");
    g->write_line("");
    g->write("  trial 0..");
    fmt_u64(nbuf, tn);
    g->write(nbuf);
    g->write("  primes=");
    fmt_u64(nbuf, tc);
    g->write(nbuf);
    g->write("  ");
    fmt_u64(nbuf, tms);
    g->write(nbuf);
    g->write_line(" ms");
    g->write("  alu ");
    fmt_u64(nbuf, aops);
    g->write(nbuf);
    g->write(" ops in ");
    fmt_u64(nbuf, ams);
    g->write(nbuf);
    g->write_line(" ms");
    g->write_line(ok ? "RESULT: PASS" : "RESULT: FAIL");
}

static void draw_cpu_frame(int which, int running) {
    int i;
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " CPU  ·  Prime numbers ");
    put_str(win_x + 3, win_y + 3, C_FG, C_WIN, "Sieve of Eratosthenes  +  trial division  +  ALU", win_w - 6);
    put_str(win_x + 3, win_y + 5, C_MUTED, C_WIN, "Limit", 8);
    for (i = 0; i < N_SIEVE; i++) {
        int x = win_x + 10 + i * 9;
        uint8_t fg = i == which ? C_SEL_FG : C_FG;
        uint8_t bg = i == which ? C_SEL_BG : C_BTN_BG;
        fill_span(x, win_y + 5, 8, fg, bg, ' ');
        put_str(x + 1, win_y + 5, fg, bg, SIEVES[i].label, 6);
    }
    footer(win_y + win_h - 2,
           running ? "Working...  q cancel" : "Left/Right pick size  Enter run  q back");
}

static void show_cpu_results(int which, uint32_t sn, uint32_t scount, uint64_t sms, int ok,
                             uint32_t tn, uint32_t tc, uint64_t tms,
                             uint64_t aops, uint64_t ams) {
    char line[80];
    int y = win_y + 11;
    uint64_t nps;

    fill_span(win_x + 3, win_y + 8, win_w - 6, C_FG, C_WIN, ' ');
    fill_span(win_x + 3, win_y + 9, win_w - 6, C_FG, C_WIN, ' ');
    bar(win_x + 4, win_y + 8, win_w - 8, 1, 1, C_ACC, C_WIN);

    put_str(win_x + 3, y, C_TITLE, C_WIN, "Sieve", 8);
    put_str(win_x + 12, y, C_MUTED, C_WIN, "primes", 8);
    put_u64(win_x + 20, y, C_ACC, C_WIN, scount);
    put_str(win_x + 32, y, C_MUTED, C_WIN, "time", 6);
    put_ms(win_x + 38, y, C_FG, C_WIN, sms);
    put_str(win_x + 52, y, ok ? C_ACC : C_HOT, C_WIN, ok ? "PASS" : "FAIL", 6);
    y++;
    if (sms == 0) {
        sms = 1;
    }
    nps = ((uint64_t)sn * 1000ull) / sms;
    put_str(win_x + 12, y, C_MUTED, C_WIN, "rate", 6);
    put_u64(win_x + 20, y, C_INFO, C_WIN, nps);
    put_str(win_x + 20 + fmt_u64(line, nps) + 1, y, C_MUTED, C_WIN, "n/s", 4);
    put_str(win_x + 40, y, C_MUTED, C_WIN, "expect", 8);
    put_u64(win_x + 48, y, C_FG, C_WIN, SIEVES[which].expect);
    y += 2;
    put_str(win_x + 3, y, C_TITLE, C_WIN, "Trial", 8);
    put_str(win_x + 12, y, C_MUTED, C_WIN, "primes", 8);
    put_u64(win_x + 20, y, C_ACC, C_WIN, tc);
    put_str(win_x + 32, y, C_MUTED, C_WIN, "time", 6);
    put_ms(win_x + 38, y, C_FG, C_WIN, tms);
    y++;
    put_str(win_x + 12, y, C_MUTED, C_WIN, "limit", 8);
    put_u64(win_x + 20, y, C_FG, C_WIN, tn);
    y += 2;
    put_str(win_x + 3, y, C_TITLE, C_WIN, "ALU", 8);
    put_u64(win_x + 12, y, C_ACC, C_WIN, aops);
    put_str(win_x + 12 + fmt_u64(line, aops) + 1, y, C_MUTED, C_WIN, "ops", 4);
    put_ms(win_x + 38, y, C_FG, C_WIN, ams);
    if (ams) {
        uint64_t kops = (aops * 1000ull) / ams;
        put_u64(win_x + 50, y, C_INFO, C_WIN, kops);
        put_str(win_x + 50 + fmt_u64(line, kops) + 1, y, C_MUTED, C_WIN, "/s", 3);
    }
    (void)scount;
}

static int run_cpu_ui(int preset) {
    int which = clampi(preset, 0, N_SIEVE - 1);
    uint32_t sn = 0, scount = 0, tn = 0, tc = 0;
    uint64_t sms = 0, tms = 0, aops = 0, ams = 0;
    int ok = 0;

    sti();
    hide_cursor();
    draw_cpu_frame(which, 0);
    drain_keys();
    for (;;) {
        int mx, my;
        int d;
        int k;

        if (mouse_click(&mx, &my)) {
            if (my == win_y + 5) {
                int i;
                for (i = 0; i < N_SIEVE; i++) {
                    int x = win_x + 10 + i * 9;
                    if (mx >= x && mx < x + 8) {
                        which = i;
                        draw_cpu_frame(which, 0);
                    }
                }
            } else if (my >= win_y + 7 && my <= win_y + 10) {
                k = IN_ENTER;
                goto handle;
            }
        }
        k = poll_in(&d);
        if (k == IN_NONE) {
            pause_cpu();
            continue;
        }
handle:
        if (k == IN_BACK) {
            return IN_BACK;
        }
        if (k == IN_LEFT) {
            which = (which + N_SIEVE - 1) % N_SIEVE;
            draw_cpu_frame(which, 0);
        } else if (k == IN_RIGHT) {
            which = (which + 1) % N_SIEVE;
            draw_cpu_frame(which, 0);
        } else if (k == IN_DIGIT && d >= 0 && d < N_SIEVE) {
            which = d;
            draw_cpu_frame(which, 0);
        } else if (k == IN_ENTER || k == IN_SPACE) {
            int rc;
            draw_cpu_frame(which, 1);
            fill_span(win_x + 3, win_y + 7, win_w - 6, C_FG, C_WIN, ' ');
            rc = cpu_execute(which, 1, &sn, &scount, &sms, &ok, &tn, &tc, &tms, &aops, &ams);
            if (rc == -1) {
                draw_cpu_frame(which, 0);
                put_str(win_x + 4, win_y + 8, C_HOT, C_WIN, "Cancelled.", 20);
                continue;
            }
            if (rc == -2) {
                draw_cpu_frame(which, 0);
                put_str(win_x + 4, win_y + 8, C_HOT, C_WIN, "Need heap (rebuild kernel).", 32);
                continue;
            }
            draw_cpu_frame(which, 0);
            show_cpu_results(which, sn, scount, sms, ok, tn, tc, tms, aops, ams);
        }
    }
}

/* ---- graphics ---- */

static int gx, gy, gw, gh;

static void gfx_viewport(void) {
    gx = win_x + 2;
    gy = win_y + 3;
    gw = win_w - 4;
    gh = win_h - 6;
    if (gw < 8) {
        gw = 8;
    }
    if (gh < 6) {
        gh = 6;
    }
}

static void gfx_clear(uint8_t bg) {
    fill_rect(gx, gy, gw, gh, C_FG, bg, ' ');
}

static void plot(int x, int y, uint8_t fg, uint8_t bg, unsigned char ch) {
    if (x < 0 || y < 0 || x >= gw || y >= gh) {
        return;
    }
    put_xy(gx + x, gy + y, fg, bg, ch);
}

static void line_to(int x0, int y0, int x1, int y1, uint8_t fg, uint8_t bg, unsigned char ch) {
    int dx = iabs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, fg, bg, ch);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void scene_plasma(int t) {
    int y, x;
    for (y = 0; y < gh; y++) {
        for (x = 0; x < gw; x++) {
            int v = isin(x * 3 + t) + isin(y * 5 + t * 2) + isin(x + y + t);
            int idx;
            uint8_t fg;
            unsigned char ch;
            v += 381;
            idx = (v * 16) / 762;
            if (idx < 0) {
                idx = 0;
            }
            if (idx > 15) {
                idx = 15;
            }
            fg = (uint8_t)idx;
            if (fg == 0) {
                fg = 1;
            }
            if (idx < 4) {
                ch = CH_LITE;
            } else if (idx < 8) {
                ch = CH_DIM;
            } else if (idx < 12) {
                ch = CH_MED;
            } else {
                ch = CH_FILL;
            }
            plot(x, y, fg, C_WIN, ch);
        }
    }
}

typedef struct {
    int16_t x, y, z;
} star_t;

#define N_STAR 80
static star_t stars[N_STAR];
static int stars_on;

static void stars_init(void) {
    int i;
    for (i = 0; i < N_STAR; i++) {
        stars[i].x = (int16_t)((int)(rnd() % 400u) - 200);
        stars[i].y = (int16_t)((int)(rnd() % 200u) - 100);
        stars[i].z = (int16_t)((rnd() % 180u) + 20);
    }
    stars_on = 1;
}

static void scene_stars(int t) {
    int i;
    int cx = gw / 2;
    int cy = gh / 2;
    (void)t;
    gfx_clear(C_WIN);
    for (i = 0; i < N_STAR; i++) {
        int z, sx, sy;
        uint8_t fg;
        unsigned char ch;
        stars[i].z = (int16_t)(stars[i].z - 3);
        if (stars[i].z < 8) {
            stars[i].x = (int16_t)((int)(rnd() % 400u) - 200);
            stars[i].y = (int16_t)((int)(rnd() % 200u) - 100);
            stars[i].z = 180;
        }
        z = stars[i].z;
        sx = cx + (stars[i].x * (gw / 3)) / z;
        sy = cy + (stars[i].y * (gh / 3)) / z;
        if (z > 120) {
            fg = 8;
            ch = CH_DOT;
        } else if (z > 60) {
            fg = 7;
            ch = '*';
        } else {
            fg = 15;
            ch = CH_STAR;
        }
        plot(sx, sy, fg, C_WIN, ch);
    }
}

static void rot3(int x, int y, int z, int ax, int ay, int az, int *ox, int *oy, int *oz) {
    int16_t c, s;
    int nx, ny, nz;

    c = icos(az);
    s = isin(az);
    nx = (x * c - y * s) / 127;
    ny = (x * s + y * c) / 127;
    x = nx;
    y = ny;

    c = icos(ay);
    s = isin(ay);
    nx = (x * c - z * s) / 127;
    nz = (x * s + z * c) / 127;
    x = nx;
    z = nz;

    c = icos(ax);
    s = isin(ax);
    ny = (y * c - z * s) / 127;
    nz = (y * s + z * c) / 127;
    *ox = x;
    *oy = ny;
    *oz = nz;
}

static void proj3(int x, int y, int z, int *sx, int *sy) {
    int zc = z + 280;
    int cx = gw / 2;
    int cy = gh / 2;
    int hx = gw / 2 - 3;
    int hy = gh / 2 - 2;
    if (zc < 32) {
        zc = 32;
    }
    if (hx < 8) {
        hx = 8;
    }
    if (hy < 4) {
        hy = 4;
    }
    *sx = cx + (x * hx) / zc;
    *sy = cy + (y * hy) / zc;
}

#define KIND_CUBE 0
#define KIND_PYR  1
#define KIND_OCT  2
#define KIND_BAR  3
#define N_BODY    5
#define ARENA_X   150
#define ARENA_Y    82
#define ARENA_Z    96

typedef struct {
    int x, y, z;
    int vx, vy, vz;
    int ax, ay, az;
    int sax, say, saz;
    int scale;
    int rad;
    int kind;
    uint8_t color;
} body_t;

static body_t bodies[N_BODY];
static int bounce_on;

static const int8_t V_CUBE[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}
};
static const uint8_t E_CUBE[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7}
};
static const int8_t V_PYR[5][3] = {
    {0, -2, 0}, {-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1}
};
static const uint8_t E_PYR[8][2] = {
    {0, 1}, {0, 2}, {0, 3}, {0, 4},
    {1, 2}, {2, 3}, {3, 4}, {4, 1}
};
static const int8_t V_OCT[6][3] = {
    {2, 0, 0}, {-2, 0, 0}, {0, 2, 0}, {0, -2, 0}, {0, 0, 2}, {0, 0, -2}
};
static const uint8_t E_OCT[12][2] = {
    {0, 2}, {0, 3}, {0, 4}, {0, 5},
    {1, 2}, {1, 3}, {1, 4}, {1, 5},
    {2, 4}, {4, 3}, {3, 5}, {5, 2}
};
static const int8_t V_BAR[8][3] = {
    {-3, -1, -1}, {3, -1, -1}, {3, 1, -1}, {-3, 1, -1},
    {-3, -1, 1},  {3, -1, 1},  {3, 1, 1},  {-3, 1, 1}
};

static void bounce_init(void) {
    static const uint8_t col[N_BODY] = {11, 14, 13, 10, 12};
    static const int kind[N_BODY] = {KIND_CUBE, KIND_PYR, KIND_OCT, KIND_BAR, KIND_CUBE};
    static const int sc[N_BODY] = {28, 26, 22, 16, 18};
    int i;

    for (i = 0; i < N_BODY; i++) {
        bodies[i].x = -90 + i * 42;
        bodies[i].y = -50 + (int)(rnd() % 40u);
        bodies[i].z = -40 + (int)(rnd() % 70u);
        bodies[i].vx = (int)(rnd() % 7u) - 3;
        bodies[i].vy = -6 - (int)(rnd() % 5u);
        bodies[i].vz = (int)(rnd() % 7u) - 3;
        if (bodies[i].vx == 0) {
            bodies[i].vx = (i & 1) ? 3 : -3;
        }
        bodies[i].ax = (int)(rnd() & 255u);
        bodies[i].ay = (int)(rnd() & 255u);
        bodies[i].az = (int)(rnd() & 255u);
        bodies[i].sax = 1 + (int)(rnd() % 3u);
        bodies[i].say = 2 + (int)(rnd() % 4u);
        bodies[i].saz = (int)(rnd() % 3u);
        bodies[i].scale = sc[i];
        bodies[i].rad = sc[i] + (kind[i] == KIND_BAR ? 10 : 4);
        bodies[i].kind = kind[i];
        bodies[i].color = col[i];
    }
    bounce_on = 1;
}

static void bounce_wall(int *p, int *v, int lim, int rad) {
    if (*p + rad > lim) {
        *p = lim - rad;
        *v = -*v;
    } else if (*p - rad < -lim) {
        *p = -lim + rad;
        *v = -*v;
    }
}

static void bounce_step(void) {
    int i, j;

    for (i = 0; i < N_BODY; i++) {
        body_t *b = &bodies[i];
        b->x += b->vx;
        b->y += b->vy;
        b->z += b->vz;
        b->vy += 1;
        if (b->vy > 12) {
            b->vy = 12;
        }
        bounce_wall(&b->x, &b->vx, ARENA_X, b->rad);
        bounce_wall(&b->z, &b->vz, ARENA_Z, b->rad);
        if (b->y + b->rad > ARENA_Y) {
            b->y = ARENA_Y - b->rad;
            b->vy = -(b->vy * 4) / 5;
            if (b->vy > -4) {
                b->vy = -8 - (int)(rnd() % 4u);
            }
            if ((rnd() & 3u) == 0) {
                b->vx += (int)(rnd() % 5u) - 2;
            }
        }
        if (b->y - b->rad < -ARENA_Y) {
            b->y = -ARENA_Y + b->rad;
            b->vy = iabs(b->vy);
        }
        b->ax = (b->ax + b->sax) & 255;
        b->ay = (b->ay + b->say) & 255;
        b->az = (b->az + b->saz) & 255;
    }

    for (i = 0; i < N_BODY; i++) {
        for (j = i + 1; j < N_BODY; j++) {
            int dx = bodies[j].x - bodies[i].x;
            int dy = bodies[j].y - bodies[i].y;
            int dz = bodies[j].z - bodies[i].z;
            int r = bodies[i].rad + bodies[j].rad;
            int d2 = dx * dx + dy * dy + dz * dz;
            int tmp;
            if (d2 == 0 || d2 >= r * r) {
                continue;
            }
            tmp = bodies[i].vx;
            bodies[i].vx = bodies[j].vx;
            bodies[j].vx = tmp;
            tmp = bodies[i].vy;
            bodies[i].vy = bodies[j].vy;
            bodies[j].vy = tmp;
            tmp = bodies[i].vz;
            bodies[i].vz = bodies[j].vz;
            bodies[j].vz = tmp;
            if (dx > 0) {
                bodies[i].x -= 3;
                bodies[j].x += 3;
            } else {
                bodies[i].x += 3;
                bodies[j].x -= 3;
            }
            if (dy > 0) {
                bodies[i].y -= 3;
                bodies[j].y += 3;
            } else {
                bodies[i].y += 3;
                bodies[j].y -= 3;
            }
        }
    }
}

static void draw_mesh(const int8_t v[][3], int nv, const uint8_t e[][2], int ne,
                     const body_t *b, int sx_mul, int sy_mul, int sz_mul) {
    int p[8][2];
    int i;
    int sx = sx_mul ? sx_mul : b->scale;
    int sy = sy_mul ? sy_mul : b->scale;
    int sz = sz_mul ? sz_mul : b->scale;

    for (i = 0; i < nv; i++) {
        int x, y, z;
        rot3(v[i][0] * sx, v[i][1] * sy, v[i][2] * sz, b->ax, b->ay, b->az, &x, &y, &z);
        proj3(x + b->x, y + b->y, z + b->z, &p[i][0], &p[i][1]);
    }
    for (i = 0; i < ne; i++) {
        int a = e[i][0];
        int b2 = e[i][1];
        line_to(p[a][0], p[a][1], p[b2][0], p[b2][1], b->color, C_WIN, CH_FILL);
    }
    for (i = 0; i < nv; i++) {
        plot(p[i][0], p[i][1], C_TITLE, C_WIN, CH_BLOCK);
    }
}

static void draw_body(const body_t *b) {
    int shx, shy;
    proj3(b->x, ARENA_Y, b->z, &shx, &shy);
    plot(shx, shy, C_MUTED, C_WIN, CH_LITE);
    plot(shx - 1, shy, C_MUTED, C_WIN, CH_DOT);
    plot(shx + 1, shy, C_MUTED, C_WIN, CH_DOT);
    if (b->kind == KIND_PYR) {
        draw_mesh(V_PYR, 5, E_PYR, 8, b, 0, 0, 0);
    } else if (b->kind == KIND_OCT) {
        draw_mesh(V_OCT, 6, E_OCT, 12, b, 0, 0, 0);
    } else if (b->kind == KIND_BAR) {
        draw_mesh(V_BAR, 8, E_CUBE, 12, b, 0, 0, 0);
    } else {
        draw_mesh(V_CUBE, 8, E_CUBE, 12, b, 0, 0, 0);
    }
}

static void draw_arena(void) {
    int c[8][2];
    int i;
    static const int8_t box[8][3] = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
        {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}
    };
    static const uint8_t ed[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    int gx0, gz;

    for (i = 0; i < 8; i++) {
        proj3(box[i][0] * ARENA_X, box[i][1] * ARENA_Y, box[i][2] * ARENA_Z,
              &c[i][0], &c[i][1]);
    }
    for (i = 0; i < 12; i++) {
        line_to(c[ed[i][0]][0], c[ed[i][0]][1], c[ed[i][1]][0], c[ed[i][1]][1],
                C_MUTED, C_WIN, CH_DIM);
    }
    for (gx0 = -ARENA_X; gx0 <= ARENA_X; gx0 += 50) {
        int a0, a1, b0, b1;
        proj3(gx0, ARENA_Y, -ARENA_Z, &a0, &a1);
        proj3(gx0, ARENA_Y, ARENA_Z, &b0, &b1);
        line_to(a0, a1, b0, b1, 8, C_WIN, CH_DOT);
    }
    for (gz = -ARENA_Z; gz <= ARENA_Z; gz += 48) {
        int a0, a1, b0, b1;
        proj3(-ARENA_X, ARENA_Y, gz, &a0, &a1);
        proj3(ARENA_X, ARENA_Y, gz, &b0, &b1);
        line_to(a0, a1, b0, b1, 8, C_WIN, CH_DOT);
    }
}

static void scene_bounce3d(int t) {
    int order[N_BODY];
    int i, j;

    (void)t;
    if (!bounce_on) {
        bounce_init();
    }
    bounce_step();
    gfx_clear(C_WIN);
    draw_arena();
    for (i = 0; i < N_BODY; i++) {
        order[i] = i;
    }
    for (i = 0; i < N_BODY - 1; i++) {
        for (j = i + 1; j < N_BODY; j++) {
            if (bodies[order[j]].z < bodies[order[i]].z) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    for (i = 0; i < N_BODY; i++) {
        draw_body(&bodies[order[i]]);
    }
}

#define FIRE_MAX (MAX_COLS * MAX_ROWS)
static uint8_t fire[FIRE_MAX];
static int fire_on;

static void fire_init(void) {
    int i;
    for (i = 0; i < FIRE_MAX; i++) {
        fire[i] = 0;
    }
    fire_on = 1;
}

static void scene_fire(int t) {
    int x, y;
    int w = gw;
    int h = gh;
    (void)t;
    if (w * h > FIRE_MAX) {
        h = FIRE_MAX / w;
    }
    for (x = 0; x < w; x++) {
        uint8_t v = (uint8_t)(180 + (rnd() % 76u));
        fire[(h - 1) * w + x] = v;
        if (h > 1) {
            fire[(h - 2) * w + x] = (uint8_t)(v > 20 ? v - 20 : 0);
        }
    }
    for (y = 0; y < h - 2; y++) {
        for (x = 0; x < w; x++) {
            int s = 0;
            int n = 0;
            int dx;
            for (dx = -1; dx <= 1; dx++) {
                int xx = x + dx;
                if (xx >= 0 && xx < w) {
                    s += fire[(y + 1) * w + xx];
                    n++;
                }
            }
            if (x >= 0 && x < w) {
                s += fire[(y + 2) * w + x];
                n++;
            }
            s = s / n - 2;
            if (s < 0) {
                s = 0;
            }
            fire[y * w + x] = (uint8_t)s;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t v = fire[y * w + x];
            uint8_t fg;
            uint8_t bg = C_WIN;
            unsigned char ch;
            if (v < 12) {
                fg = 0;
                ch = ' ';
            } else if (v < 40) {
                fg = 4;
                ch = CH_LITE;
            } else if (v < 80) {
                fg = 12;
                ch = CH_DIM;
            } else if (v < 130) {
                fg = 14;
                ch = CH_MED;
            } else if (v < 190) {
                fg = 14;
                bg = 4;
                ch = CH_FILL;
            } else {
                fg = 15;
                bg = 14;
                ch = CH_FILL;
            }
            plot(x, y, fg, bg, ch);
        }
    }
}

static const char *GFX_NAME[N_GFX] = {
    "Plasma", "Starfield", "3D bounce", "Fire"
};

static void draw_gfx_chrome(int scene) {
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " GFX  ·  Graphics demo ");
    put_str(win_x + 3, win_y + 1, C_TITLE, C_BTN_BG, GFX_NAME[scene], 16);
    footer(win_y + win_h - 2, "1-4 / Left Right  scene   Space pause   q back");
    gfx_viewport();
}

static int run_gfx_ui(int start_scene, uint32_t auto_ms) {
    int scene = clampi(start_scene, 0, N_GFX - 1);
    int t = 0;
    int paused = 0;
    uint64_t t0 = now_ms();
    uint64_t last = 0;

    sti();
    hide_cursor();
    stars_on = 0;
    fire_on = 0;
    bounce_on = 0;
    draw_gfx_chrome(scene);
    drain_keys();
    for (;;) {
        int d;
        int k;
        uint64_t n = now_ms();

        if (auto_ms && n - t0 >= auto_ms) {
            return IN_ENTER;
        }
        k = poll_in(&d);
        if (k == IN_BACK) {
            return IN_BACK;
        }
        if (k == IN_LEFT) {
            scene = (scene + N_GFX - 1) % N_GFX;
            draw_gfx_chrome(scene);
        } else if (k == IN_RIGHT) {
            scene = (scene + 1) % N_GFX;
            draw_gfx_chrome(scene);
        } else if (k == IN_DIGIT && d >= 0 && d < N_GFX) {
            scene = d;
            draw_gfx_chrome(scene);
        } else if (k == IN_SPACE) {
            paused = !paused;
        } else if (k == IN_ENTER && auto_ms) {
            return IN_ENTER;
        }

        if (paused) {
            pause_cpu();
            continue;
        }
        if (n - last < 16u) {
            pause_cpu();
            continue;
        }
        last = n;
        t++;
        if (scene == 0) {
            scene_plasma(t);
        } else if (scene == 1) {
            if (!stars_on) {
                stars_init();
            }
            scene_stars(t);
        } else if (scene == 2) {
            scene_bounce3d(t);
        } else {
            if (!fire_on) {
                fire_init();
            }
            scene_fire(t);
        }
        if (g->has_key) {
            (void)g->has_key();
        }
    }
}

/* ---- audio ---- */

static const uint16_t SCALE_HZ[8] = {262, 294, 330, 349, 392, 440, 494, 523};
static const char *SCALE_NM[8] = {"C", "D", "E", "F", "G", "A", "B", "C"};

/* Beethoven, Ode to Joy (public domain). */
static const uint16_t JOY_HZ[] = {
    330, 330, 349, 392, 392, 349, 330, 294,
    262, 262, 294, 330, 330, 294, 294, 0,
    330, 330, 349, 392, 392, 349, 330, 294,
    262, 262, 294, 330, 294, 262, 262, 0
};
#define JOY_N ((int)(sizeof(JOY_HZ) / sizeof(JOY_HZ[0])))

static int key_for_hz(uint16_t hz) {
    int i;
    for (i = 0; i < 8; i++) {
        if (SCALE_HZ[i] == hz) {
            return i;
        }
    }
    if (hz == 0) {
        return -1;
    }
    {
        int best = 0;
        int bd = 9999;
        for (i = 0; i < 8; i++) {
            int d = iabs((int)SCALE_HZ[i] - (int)hz);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        return best;
    }
}

static void draw_piano(int lit) {
    int i;
    int kw;
    int x0;
    int y = win_y + win_h - 6;
    kw = (win_w - 8) / 8;
    if (kw < 5) {
        kw = 5;
    }
    x0 = win_x + 4;
    for (i = 0; i < 8; i++) {
        int x = x0 + i * kw;
        uint8_t fg = (i == lit) ? C_SEL_FG : C_CARD_FG;
        uint8_t bg = (i == lit) ? C_SEL_BG : C_CARD;
        fill_span(x, y, kw - 1, fg, bg, ' ');
        fill_span(x, y + 1, kw - 1, fg, bg, ' ');
        put_str(x + (kw - 2) / 2, y + 1, fg, bg, SCALE_NM[i], 1);
    }
}

static void draw_snd_frame(const char *status, int lit) {
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " SND  ·  Audio demo ");
    put_xy(win_x + 3, win_y + 3, C_TITLE, C_WIN, CH_NOTE);
    put_str(win_x + 5, win_y + 3, C_FG, C_WIN, "Sound Blaster 16  ·  8-bit PCM", win_w - 8);
    put_str(win_x + 3, win_y + 5, C_MUTED, C_WIN, "1  Scale     2  Ode to Joy     3  Dual-voice synth", win_w - 6);
    fill_span(win_x + 3, win_y + 7, win_w - 6, C_INFO, C_WIN, ' ');
    if (status) {
        put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, status, win_w - 6);
    }
    draw_piano(lit);
    footer(win_y + win_h - 2, "1-3 play   q back");
}

static int snd_ready(void) {
    if (!g->sound_present || !g->sound_beep || !g->sound_play) {
        if (g->show_error) {
            g->show_error("Sound API missing — rebuild the kernel");
        }
        return 0;
    }
    if (!g->sound_present()) {
        if (g->show_error) {
            g->show_error("No Sound Blaster (QEMU: -device sb16)");
        }
        return 0;
    }
    return 1;
}

static int cancelled(void) {
    int d;
    return poll_in(&d) == IN_BACK;
}

static int play_scale(void) {
    int i;
    draw_snd_frame("C major scale", -1);
    for (i = 0; i < 8; i++) {
        if (cancelled()) {
            if (g->sound_stop) {
                g->sound_stop();
            }
            return IN_BACK;
        }
        draw_piano(i);
        put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, "Playing scale...", 24);
        if (g->sound_beep(SCALE_HZ[i], 160) != 0) {
            put_str(win_x + 3, win_y + 7, C_HOT, C_WIN, "Playback failed", 20);
            return IN_ENTER;
        }
    }
    draw_piano(-1);
    put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, "Scale done.", 16);
    return IN_ENTER;
}

static int play_joy(void) {
    int i;
    draw_snd_frame("Ode to Joy", -1);
    for (i = 0; i < JOY_N; i++) {
        int lit;
        if (cancelled()) {
            if (g->sound_stop) {
                g->sound_stop();
            }
            return IN_BACK;
        }
        lit = key_for_hz(JOY_HZ[i]);
        draw_piano(lit);
        if (JOY_HZ[i] == 0) {
            if (g->sleep_ms) {
                g->sleep_ms(120);
            }
            continue;
        }
        if (g->sound_beep(JOY_HZ[i], 180) != 0) {
            put_str(win_x + 3, win_y + 7, C_HOT, C_WIN, "Playback failed", 20);
            return IN_ENTER;
        }
    }
    draw_piano(-1);
    put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, "Melody done.", 16);
    return IN_ENTER;
}

#define SYNTH_RATE 8000u
#define SYNTH_N    16000u

static int play_synth(void) {
    uint8_t *pcm;
    uint32_t i;
    uint32_t ph1 = 0, ph2 = 0;
    uint32_t step1, step2;
    int bw;
    int x;
    int y;
    static const uint16_t melody[] = {330, 392, 523, 392, 440, 392, 330, 262};
    static const uint16_t bass[] = {131, 131, 196, 165, 175, 165, 131, 98};

    if (!g->mem_alloc) {
        put_str(win_x + 3, win_y + 7, C_HOT, C_WIN, "Need heap for synth buffer.", 32);
        return IN_ENTER;
    }
    pcm = (uint8_t *)g->mem_alloc(SYNTH_N);
    if (!pcm) {
        put_str(win_x + 3, win_y + 7, C_HOT, C_WIN, "Out of memory.", 20);
        return IN_ENTER;
    }
    draw_snd_frame("Rendering dual-voice synth...", -1);
    for (i = 0; i < SYNTH_N; i++) {
        uint32_t note = (i * 8u) / SYNTH_N;
        int16_t a, b;
        int16_t mix;
        uint32_t env;
        uint32_t pos;
        if (note > 7) {
            note = 7;
        }
        step1 = (uint32_t)melody[note] * 256u / SYNTH_RATE;
        step2 = (uint32_t)bass[note] * 256u / SYNTH_RATE;
        ph1 += step1;
        ph2 += step2;
        a = isin((int)(ph1 & 255u));
        b = isin((int)(ph2 & 255u));
        pos = i % (SYNTH_N / 8u);
        env = 120;
        if (pos < 400) {
            env = (pos * 120u) / 400u;
        } else if (pos > (SYNTH_N / 8u) - 600u) {
            uint32_t left = (SYNTH_N / 8u) - pos;
            env = (left * 120u) / 600u;
        }
        mix = (int16_t)(((a * (int)env) + (b * (int)env / 2)) / 127);
        if (mix < -120) {
            mix = -120;
        }
        if (mix > 120) {
            mix = 120;
        }
        pcm[i] = (uint8_t)(128 + mix);
        if ((i & 2047u) == 0 && cancelled()) {
            g->mem_free(pcm);
            return IN_BACK;
        }
    }

    /* Oscilloscope of the first window of samples. */
    bw = win_w - 8;
    y = win_y + 9;
    fill_span(win_x + 3, y, win_w - 6, C_MUTED, C_WIN, CH_LITE);
    fill_span(win_x + 3, y + 1, win_w - 6, C_MUTED, C_WIN, ' ');
    fill_span(win_x + 3, y + 2, win_w - 6, C_MUTED, C_WIN, CH_LITE);
    for (x = 0; x < bw; x++) {
        uint32_t idx = (uint32_t)x * SYNTH_N / (uint32_t)bw;
        int v = (int)pcm[idx] - 128;
        int row = v >= 20 ? 0 : (v <= -20 ? 2 : 1);
        put_xy(win_x + 4 + x, y + row, C_ACC, C_WIN, CH_FILL);
    }
    draw_piano(2);
    put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, "Playing synth (2 s)...", 28);
    if (g->sound_play(pcm, SYNTH_N, SYNTH_RATE) != 0) {
        put_str(win_x + 3, win_y + 7, C_HOT, C_WIN, "Playback failed", 20);
    } else {
        put_str(win_x + 3, win_y + 7, C_ACC, C_WIN, "Synth done.", 16);
    }
    g->mem_free(pcm);
    draw_piano(-1);
    return IN_ENTER;
}

static int run_snd_ui(int auto_which) {
    int which = -1;
    sti();
    hide_cursor();
    if (!snd_ready()) {
        return IN_BACK;
    }
    draw_snd_frame("Pick a demo.", -1);
    drain_keys();
    if (auto_which >= 0) {
        which = auto_which;
        goto play;
    }
    for (;;) {
        int d;
        int k = poll_in(&d);
        int mx, my;
        if (mouse_click(&mx, &my)) {
            if (my == win_y + 5) {
                if (mx < win_x + 18) {
                    which = 0;
                } else if (mx < win_x + 40) {
                    which = 1;
                } else {
                    which = 2;
                }
                goto play;
            }
        }
        if (k == IN_NONE) {
            pause_cpu();
            continue;
        }
        if (k == IN_BACK) {
            return IN_BACK;
        }
        if (k == IN_DIGIT && d >= 0 && d < N_SND) {
            which = d;
            goto play;
        }
        if (k == IN_ENTER || k == IN_SPACE) {
            which = 0;
            goto play;
        }
        continue;
play:
        {
            int rc;
            if (which == 0) {
                rc = play_scale();
            } else if (which == 1) {
                rc = play_joy();
            } else {
                rc = play_synth();
            }
            if (auto_which >= 0) {
                return rc;
            }
            if (rc == IN_BACK) {
                draw_snd_frame("Stopped.", -1);
            } else {
                draw_snd_frame("Pick a demo.", -1);
            }
            drain_keys();
            which = -1;
        }
    }
}

/* ---- run all ---- */

static int run_all(void) {
    uint32_t sn, sc, tn, tc;
    uint64_t sms, tms, aops, ams;
    int ok;
    int i;
    int rc;

    sti();
    hide_cursor();
    draw_cpu_frame(0, 1);
    put_str(win_x + 4, win_y + 8, C_INFO, C_WIN, "Full show — 100k sieve first...", 36);
    rc = cpu_execute(0, 1, &sn, &sc, &sms, &ok, &tn, &tc, &tms, &aops, &ams);
    if (rc == -1) {
        return IN_BACK;
    }
    draw_cpu_frame(0, 0);
    if (rc == 0) {
        show_cpu_results(0, sn, sc, sms, ok, tn, tc, tms, aops, ams);
    }
    {
        uint64_t t0 = now_ms();
        while (now_ms() - t0 < 1200u) {
            int d;
            if (poll_in(&d) == IN_BACK) {
                return IN_BACK;
            }
            pause_cpu();
        }
    }
    for (i = 0; i < N_GFX; i++) {
        if (run_gfx_ui(i, 2200u) == IN_BACK) {
            return IN_BACK;
        }
    }
    if (snd_ready()) {
        if (play_scale() == IN_BACK) {
            return IN_BACK;
        }
        if (play_joy() == IN_BACK) {
            return IN_BACK;
        }
    }
    layout_menu();
    desktop();
    draw_window(win_x, win_y, win_w, 11, " Show complete ");
    put_str(win_x + 4, win_y + 4, C_ACC, C_WIN, ok ? "Prime sieve: PASS" : "Prime sieve: FAIL", 24);
    put_str(win_x + 4, win_y + 6, C_FG, C_WIN, "Graphics + audio played.", 28);
    footer(win_y + 9, "Enter / q  back to menu");
    drain_keys();
    for (;;) {
        int k = wait_in();
        if (k == IN_BACK || k == IN_ENTER || k == IN_SPACE) {
            return IN_ENTER;
        }
    }
}

/* ---- main menu ---- */

typedef struct {
    char key;
    const char *tag;
    const char *title;
    const char *blurb;
} menu_item_t;

static const menu_item_t MENU[N_MENU] = {
    {'1', "CPU", "Prime numbers", "Sieve, trial division, integer ALU"},
    {'2', "GFX", "Graphics demo", "Plasma, starfield, bouncing 3D, fire"},
    {'3', "SND", "Audio demo", "Scale, Ode to Joy, dual-voice synth"},
    {'4', "ALL", "Run everything", "CPU, then each graphics scene, then sound"},
    {'Q', "OUT", "Quit", "Return to the shell"}
};

static void draw_menu(int sel) {
    int i;
    int list_y;
    layout_menu();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " FOS BENCH ");
    put_str(win_x + 3, win_y + 3, C_MUTED, C_WIN, "tests  ·  benchmarks  ·  demos", win_w - 6);
    fill_span(win_x + 3, win_y + 4, win_w - 6, C_FRAME, C_WIN, CH_H);
    list_y = win_y + 6;
    for (i = 0; i < N_MENU; i++) {
        int y = list_y + i * 2;
        uint8_t fg = C_FG;
        uint8_t bg = C_WIN;
        char mark[4];
        if (i == sel) {
            fg = C_SEL_FG;
            bg = C_SEL_BG;
            fill_span(win_x + 2, y, win_w - 4, fg, bg, ' ');
            fill_span(win_x + 2, y + 1, win_w - 4, fg, bg, ' ');
            put_xy(win_x + 3, y, fg, bg, CH_PTR);
        } else {
            fill_span(win_x + 2, y, win_w - 4, C_FG, C_WIN, ' ');
            fill_span(win_x + 2, y + 1, win_w - 4, C_MUTED, C_WIN, ' ');
        }
        mark[0] = MENU[i].key;
        mark[1] = 0;
        put_str(win_x + 5, y, i == sel ? fg : C_TITLE, bg, mark, 1);
        put_str(win_x + 8, y, i == sel ? fg : C_FRAME, bg, MENU[i].tag, 4);
        put_str(win_x + 13, y, fg, bg, MENU[i].title, win_w - 16);
        put_str(win_x + 13, y + 1, i == sel ? fg : C_MUTED, bg, MENU[i].blurb, win_w - 16);
    }
    footer(win_y + win_h - 2, "Arrows / mouse  Enter  1-4  q quit");
}

static int menu_hit(int mx, int my) {
    int i;
    int list_y = win_y + 6;
    if (mx < win_x + 2 || mx >= win_x + win_w - 2) {
        return -1;
    }
    for (i = 0; i < N_MENU; i++) {
        int y = list_y + i * 2;
        if (my == y || my == y + 1) {
            return i;
        }
    }
    return -1;
}

static void restore_console(void) {
    if (g->set_color) {
        g->set_color(15, 0);
    }
    show_cursor();
    if (g->end_direct) {
        g->end_direct();
    }
}

void com_main(void) {
    const char *arg;
    int sel = 0;
    int start = -1;

    g = (fos_api_t *)FOS_API_ADDR;
    if (!g || g->magic != FOS_API_MAGIC) {
        return;
    }
    cols = 80;
    rows = 25;
    if (g->get_term_size) {
        g->get_term_size(&cols, &rows);
    }
    if (cols < 40) {
        cols = 40;
    }
    if (cols > MAX_COLS) {
        cols = MAX_COLS;
    }
    if (rows < 16) {
        rows = 16;
    }
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }

    arg = skip_ws(g->cmdline);
    if (arg[0] && (scmp(arg, "primes") == 0 || scmp(arg, "cpu") == 0 ||
                   scmp(arg, "prime") == 0)) {
        cpu_print_stdout(0);
        return;
    }
    if (arg[0] && (scmp(arg, "gfx") == 0 || scmp(arg, "graphics") == 0)) {
        start = 1;
    } else if (arg[0] && (scmp(arg, "snd") == 0 || scmp(arg, "audio") == 0 ||
                          scmp(arg, "sound") == 0)) {
        start = 2;
    } else if (arg[0] && scmp(arg, "all") == 0) {
        start = 3;
    }

    if (g->begin_direct) {
        g->begin_direct();
    }
    hide_cursor();
    sti();
    rng_state ^= (uint32_t)now_ms();
    if (rng_state == 0) {
        rng_state = 2463534242u;
    }

    if (start == 1) {
        (void)run_gfx_ui(0, 0);
        restore_console();
        return;
    }
    if (start == 2) {
        (void)run_snd_ui(-1);
        restore_console();
        return;
    }
    if (start == 3) {
        (void)run_all();
        restore_console();
        return;
    }

    draw_menu(sel);
    drain_keys();
    for (;;) {
        int d;
        int k;
        int mx, my;

        if (mouse_click(&mx, &my)) {
            int hit = menu_hit(mx, my);
            if (hit >= 0) {
                if (hit == sel) {
                    k = IN_ENTER;
                    d = -1;
                    goto dispatch;
                }
                sel = hit;
                draw_menu(sel);
            }
            continue;
        }
        k = poll_in(&d);
        if (k == IN_NONE) {
            pause_cpu();
            continue;
        }
dispatch:
        if (k == IN_BACK) {
            break;
        }
        if (k == IN_UP) {
            sel = (sel + N_MENU - 1) % N_MENU;
            draw_menu(sel);
        } else if (k == IN_DOWN) {
            sel = (sel + 1) % N_MENU;
            draw_menu(sel);
        } else if (k == IN_DIGIT && d >= 0 && d < 4) {
            sel = d;
            k = IN_ENTER;
        }
        if (k == IN_ENTER || k == IN_SPACE) {
            if (sel == 4) {
                break;
            }
            if (sel == 0) {
                (void)run_cpu_ui(3);
            } else if (sel == 1) {
                (void)run_gfx_ui(0, 0);
            } else if (sel == 2) {
                (void)run_snd_ui(-1);
            } else if (sel == 3) {
                (void)run_all();
            }
            draw_menu(sel);
            drain_keys();
        }
    }
    restore_console();
}
