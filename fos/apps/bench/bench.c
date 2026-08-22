/*
 * bench.c — FOS test bench: TUI menu, prime sieve, graphics, audio.
 *
 *   bench              interactive menu
 *   bench primes       headless CPU/prime test (for smoke)
 *   bench mem          headless heap + RAM pattern test
 *   bench burn         60 s integer soak (q cancels in the TUI)
 *   bench hw           live hardware monitor (CPU %, RAM, heap, clock)
 *   bench gfx          graphics (opens on the spinning cube)
 *
 * Arrows / 1-7 / mouse select, Enter run, q back or quit.
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

#define N_MENU 8
#define N_GFX  9
#define N_SND  3
#define BURN_MS 60000u
#define MEM_PTRS 64
#define HW_HIST 48
#define HW_FRAME_MS 200u

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

static int isqrt(int n) {
    int x, y;
    if (n <= 0) {
        return 0;
    }
    x = n;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

static int arg_is(const char *arg, const char *name) {
    if (!arg || !name) {
        return 0;
    }
    while (*name && *arg == *name) {
        arg++;
        name++;
    }
    return *name == 0 && (*arg == 0 || *arg == ' ' || *arg == '\t' ||
                          *arg == '\r' || *arg == '\n');
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

/* Shell launches BENCH inside direct VGA, which mutes COM1. Headless
 * tests need the serial console (and the restored scrollback) back. */
static void bench_use_stdout(void) {
    if (g->end_direct) {
        g->end_direct();
    }
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

static void put_size(int x, int y, uint8_t fg, uint8_t bg, uint64_t bytes) {
    char buf[40];
    const char *unit = "B";
    uint64_t v = bytes;
    int n;
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        v = bytes / (1024ull * 1024ull * 1024ull);
        unit = "GB";
    } else if (bytes >= 1024ull * 1024ull) {
        v = bytes / (1024ull * 1024ull);
        unit = "MB";
    } else if (bytes >= 1024ull) {
        v = bytes / 1024ull;
        unit = "KB";
    }
    n = fmt_u64(buf, v);
    buf[n++] = ' ';
    buf[n++] = unit[0];
    if (unit[1]) {
        buf[n++] = unit[1];
    }
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
    win_h = 9 + N_MENU * 2;
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

    bench_use_stdout();
    g->write_line("FOS Bench - prime test");
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

#define BURN_WALK 64

static uint64_t burn_spin(uint32_t ms, int live, int *cancelled) {
    uint64_t t0 = now_ms();
    uint64_t last = 0;
    uint64_t acc = 0x9E3779B97F4A7C15ULL;
    uint64_t n = 0;
    uint32_t walk[BURN_WALK];
    int i;

    if (cancelled) {
        *cancelled = 0;
    }
    for (i = 0; i < BURN_WALK; i++) {
        walk[i] = 1u + (uint32_t)i * 17u;
    }
    sti();
    while (now_ms() - t0 < (uint64_t)ms) {
        uint32_t k;
        uint32_t x;
        uint32_t d;
        for (k = 0; k < 512; k++) {
            acc *= 6364136223846793005ULL;
            acc += 1;
            acc ^= acc >> 17;
            walk[k & (BURN_WALK - 1)] += (uint32_t)acc;
            walk[k & (BURN_WALK - 1)] *= 1664525u;
            n++;
        }
        x = ((uint32_t)acc) | 1u;
        for (d = 3; d < 97; d += 2) {
            if ((x % d) == 0) {
                acc ^= d;
            }
        }
        if (now_ms() - last > 80u) {
            uint64_t el;
            int digit;
            last = now_ms();
            el = now_ms() - t0;
            if (el > ms) {
                el = ms;
            }
            if (live) {
                bar(win_x + 4, win_y + 8, win_w - 8, (uint32_t)el, ms, C_HOT, C_WIN);
                put_str(win_x + 4, win_y + 10, C_MUTED, C_WIN, "elapsed", 8);
                put_ms(win_x + 14, win_y + 10, C_FG, C_WIN, el);
                put_str(win_x + 4, win_y + 11, C_MUTED, C_WIN, "ops", 8);
                put_u64(win_x + 14, win_y + 11, C_ACC, C_WIN, n);
                if (el) {
                    put_str(win_x + 4, win_y + 12, C_MUTED, C_WIN, "rate", 8);
                    put_u64(win_x + 14, win_y + 12, C_INFO, C_WIN, (n * 1000ull) / el);
                    put_str(win_x + 36, win_y + 12, C_MUTED, C_WIN, "/s", 3);
                }
            }
            if (poll_in(&digit) == IN_BACK) {
                if (cancelled) {
                    *cancelled = 1;
                }
                break;
            }
        }
    }
    (void)acc;
    (void)walk[0];
    return n;
}

static void draw_burn_frame(int running) {
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " CPU  ·  One-minute soak ");
    put_str(win_x + 3, win_y + 3, C_FG, C_WIN,
            "Integer multiply, xor-shift, and trial mods - no pauses.", win_w - 6);
    put_str(win_x + 3, win_y + 5, C_MUTED, C_WIN, "Target", 8);
    put_ms(win_x + 12, win_y + 5, C_TITLE, C_WIN, BURN_MS);
    footer(win_y + win_h - 2,
           running ? "Burning...  q cancel" : "Enter start  q back");
}

static void burn_print_stdout(uint32_t ms) {
    int cancelled = 0;
    uint64_t t0;
    uint64_t ops;
    uint64_t dt;
    char nbuf[32];

    bench_use_stdout();
    g->write_line("FOS Bench - CPU soak");
    t0 = now_ms();
    ops = burn_spin(ms, 0, &cancelled);
    dt = now_ms() - t0;
    if (dt == 0) {
        dt = 1;
    }
    g->write("  ops=");
    fmt_u64(nbuf, ops);
    g->write(nbuf);
    g->write("  ");
    fmt_u64(nbuf, dt);
    g->write(nbuf);
    g->write_line(" ms");
    g->write("  rate=");
    fmt_u64(nbuf, (ops * 1000ull) / dt);
    g->write(nbuf);
    g->write_line("/s");
    g->write_line(cancelled ? "RESULT: CANCEL" : "RESULT: PASS");
}

static int run_burn_ui(uint32_t ms) {
    sti();
    hide_cursor();
    draw_burn_frame(0);
    drain_keys();
    for (;;) {
        int d;
        int k = poll_in(&d);
        if (k == IN_NONE) {
            pause_cpu();
            continue;
        }
        if (k == IN_BACK) {
            return IN_BACK;
        }
        if (k == IN_ENTER || k == IN_SPACE) {
            uint64_t t0;
            uint64_t ops;
            uint64_t dt;
            int cancelled = 0;
            draw_burn_frame(1);
            t0 = now_ms();
            ops = burn_spin(ms, 1, &cancelled);
            dt = now_ms() - t0;
            if (dt == 0) {
                dt = 1;
            }
            draw_burn_frame(0);
            bar(win_x + 4, win_y + 8, win_w - 8, cancelled ? (uint32_t)dt : ms, ms,
                cancelled ? C_HOT : C_ACC, C_WIN);
            put_str(win_x + 4, win_y + 10, C_MUTED, C_WIN, "elapsed", 8);
            put_ms(win_x + 14, win_y + 10, C_FG, C_WIN, dt);
            put_str(win_x + 4, win_y + 11, C_MUTED, C_WIN, "ops", 8);
            put_u64(win_x + 14, win_y + 11, C_ACC, C_WIN, ops);
            put_str(win_x + 4, win_y + 12, C_MUTED, C_WIN, "rate", 8);
            put_u64(win_x + 14, win_y + 12, C_INFO, C_WIN, (ops * 1000ull) / dt);
            put_str(win_x + 36, win_y + 12, C_MUTED, C_WIN, "/s", 3);
            put_str(win_x + 4, win_y + 14, cancelled ? C_HOT : C_ACC, C_WIN,
                    cancelled ? "Cancelled." : "PASS - soak finished.", 28);
        }
    }
}

/* ---- memory ---- */

static void *mem_ptrs[MEM_PTRS];
static uint32_t mem_sizes[MEM_PTRS];

static void mem_pat_fill(void *p, uint32_t n, uint8_t seed) {
    uint8_t *b = (uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        b[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static int mem_pat_ok(const void *p, uint32_t n, uint8_t seed) {
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (b[i] != (uint8_t)(seed + (uint8_t)i)) {
            return 0;
        }
    }
    return 1;
}

static uint32_t mem_word_pat(uint32_t i, int kind) {
    if (kind == 0) {
        return 0;
    }
    if (kind == 1) {
        return 0xFFFFFFFFu;
    }
    if (kind == 2) {
        return 0x55555555u;
    }
    if (kind == 3) {
        return 0xAAAAAAAAu;
    }
    return i * 0x9E3779B1u;
}

static int mem_march(uint8_t *p, uint32_t n, int kind, int live) {
    uint32_t *w = (uint32_t *)p;
    uint32_t nw = n / 4u;
    uint32_t i;
    uint64_t last = 0;

    for (i = 0; i < nw; i++) {
        w[i] = mem_word_pat(i, kind);
        if (live && (i & 0xFFFFu) == 0 && now_ms() - last > 50u) {
            int d;
            last = now_ms();
            bar(win_x + 4, win_y + 8, win_w - 8, i, nw ? nw : 1, C_INFO, C_WIN);
            if (poll_in(&d) == IN_BACK) {
                return -1;
            }
        }
    }
    for (i = 0; i < nw; i++) {
        if (w[i] != mem_word_pat(i, kind)) {
            return 0;
        }
        if (live && (i & 0xFFFFu) == 0 && now_ms() - last > 50u) {
            int d;
            last = now_ms();
            bar(win_x + 4, win_y + 8, win_w - 8, i, nw ? nw : 1, C_ACC, C_WIN);
            if (poll_in(&d) == IN_BACK) {
                return -1;
            }
        }
    }
    return 1;
}

static void mem_row(int row, const char *name, int ok) {
    int y = win_y + 11 + row;
    uint8_t fg;
    if (y >= win_y + win_h - 2 || y < 0) {
        return;
    }
    fg = (ok < 0) ? C_MUTED : (ok ? C_ACC : C_HOT);
    fill_span(win_x + 3, y, win_w - 6, C_FG, C_WIN, ' ');
    put_str(win_x + 4, y, fg, C_WIN, ok < 0 ? "...." : (ok ? "ok  " : "FAIL"), 4);
    put_str(win_x + 10, y, C_FG, C_WIN, name, win_w - 14);
}

static void draw_mem_frame(int running, uint64_t usable, uint64_t heap_pool) {
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " RAM  ·  Memory test ");
    put_str(win_x + 3, win_y + 3, C_FG, C_WIN,
            "Heap stress + marching patterns on a large block.", win_w - 6);
    put_str(win_x + 3, win_y + 5, C_MUTED, C_WIN, "Usable", 8);
    if (usable) {
        put_size(win_x + 12, win_y + 5, C_ACC, C_WIN, usable);
    } else {
        put_str(win_x + 12, win_y + 5, C_MUTED, C_WIN, "(no map)", 10);
    }
    put_str(win_x + 28, win_y + 5, C_MUTED, C_WIN, "heap pool", 10);
    put_size(win_x + 39, win_y + 5, C_INFO, C_WIN, heap_pool);
    footer(win_y + win_h - 2,
           running ? "Testing...  q cancel" : "Enter run  q back");
}

static int mem_execute(int live, int *npass, int *nfail, uint32_t *chunk) {
    fos_heap_info_t before;
    fos_heap_info_t after;
    uint32_t try_sz[] = {
        16u * 1024u * 1024u, 8u * 1024u * 1024u, 4u * 1024u * 1024u,
        1024u * 1024u
    };
    uint8_t *big = 0;
    uint32_t big_n = 0;
    int i;
    int row = 0;
    int ok;

    *npass = 0;
    *nfail = 0;
    *chunk = 0;
    if (!g->mem_alloc || !g->mem_free || !g->get_heap_info) {
        return -2;
    }
    g->get_heap_info(&before);

    for (i = 0; i < MEM_PTRS; i++) {
        mem_sizes[i] = (rnd() % 4096u) + 16u;
        mem_ptrs[i] = g->mem_alloc(mem_sizes[i]);
        if (mem_ptrs[i]) {
            mem_pat_fill(mem_ptrs[i], mem_sizes[i], (uint8_t)i);
        }
    }
    ok = 1;
    for (i = 0; i < MEM_PTRS; i++) {
        if (!mem_ptrs[i] || !mem_pat_ok(mem_ptrs[i], mem_sizes[i], (uint8_t)i)) {
            ok = 0;
        }
    }
    if (live) {
        mem_row(row, "mixed heap blocks hold their patterns", ok);
    }
    row++;
    if (ok) {
        (*npass)++;
    } else {
        (*nfail)++;
    }

    for (i = 0; i < MEM_PTRS; i += 2) {
        g->mem_free(mem_ptrs[i]);
        mem_ptrs[i] = 0;
    }
    for (i = 0; i < MEM_PTRS; i += 2) {
        mem_sizes[i] = (rnd() % 2048u) + 16u;
        mem_ptrs[i] = g->mem_alloc(mem_sizes[i]);
        if (mem_ptrs[i]) {
            mem_pat_fill(mem_ptrs[i], mem_sizes[i], (uint8_t)(i + 9));
        }
    }
    ok = 1;
    for (i = 0; i < MEM_PTRS; i++) {
        uint8_t seed = (i % 2) ? (uint8_t)i : (uint8_t)(i + 9);
        if (!mem_ptrs[i] || !mem_pat_ok(mem_ptrs[i], mem_sizes[i], seed)) {
            ok = 0;
        }
    }
    if (live) {
        mem_row(row, "freed holes reused, neighbours intact", ok);
    }
    row++;
    if (ok) {
        (*npass)++;
    } else {
        (*nfail)++;
    }

    {
        void *p = g->mem_alloc(64);
        ok = p != 0;
        if (ok) {
            mem_pat_fill(p, 64, 0xA5);
            if (g->mem_realloc) {
                void *q = g->mem_realloc(p, 4096);
                ok = q && mem_pat_ok(q, 64, 0xA5);
                g->mem_free(q);
            } else {
                g->mem_free(p);
            }
        }
        if (live) {
            mem_row(row, "realloc keeps the old bytes", ok);
        }
        row++;
        if (ok) {
            (*npass)++;
        } else {
            (*nfail)++;
        }
    }

    for (i = 0; i < 4; i++) {
        big = (uint8_t *)g->mem_alloc(try_sz[i]);
        if (big) {
            big_n = try_sz[i];
            break;
        }
    }
    ok = big != 0;
    if (live) {
        mem_row(row, "large block allocated", ok);
    }
    row++;
    if (ok) {
        (*npass)++;
        *chunk = big_n;
    } else {
        (*nfail)++;
    }

    if (big) {
        static const char *names[] = {
            "march 0x00", "march 0xFF", "march 0x55", "march 0xAA", "march index"
        };
        for (i = 0; i < 5; i++) {
            int rc = mem_march(big, big_n, i, live);
            if (rc < 0) {
                g->mem_free(big);
                for (i = 0; i < MEM_PTRS; i++) {
                    g->mem_free(mem_ptrs[i]);
                    mem_ptrs[i] = 0;
                }
                return -1;
            }
            ok = rc == 1;
            if (live) {
                mem_row(row, names[i], ok);
            }
            row++;
            if (ok) {
                (*npass)++;
            } else {
                (*nfail)++;
            }
        }
        g->mem_free(big);
    }

    {
        void *p = g->mem_alloc(1024ull * 1024ull * 1024ull);
        ok = p == 0;
        if (p) {
            g->mem_free(p);
        }
        if (live) {
            mem_row(row, "1 GB request refused", ok);
        }
        row++;
        if (ok) {
            (*npass)++;
        } else {
            (*nfail)++;
        }
    }

    for (i = 0; i < MEM_PTRS; i++) {
        g->mem_free(mem_ptrs[i]);
        mem_ptrs[i] = 0;
    }
    g->get_heap_info(&after);
    ok = after.heap_used == before.heap_used &&
         after.heap_blocks == before.heap_blocks;
    if (live) {
        mem_row(row, "heap back to baseline after free", ok);
    }
    if (ok) {
        (*npass)++;
    } else {
        (*nfail)++;
    }
    (void)row;
    return 0;
}

static void mem_print_stdout(void) {
    int pass = 0;
    int fail = 0;
    uint32_t chunk = 0;
    int rc;
    char nbuf[32];

    bench_use_stdout();
    g->write_line("FOS Bench - memory test");
    rc = mem_execute(0, &pass, &fail, &chunk);
    if (rc == -2) {
        g->write_line("FAIL: heap API missing");
        g->write_line("RESULT: FAIL");
        return;
    }
    g->write("  pass=");
    fmt_u64(nbuf, (uint64_t)pass);
    g->write(nbuf);
    g->write("  fail=");
    fmt_u64(nbuf, (uint64_t)fail);
    g->write(nbuf);
    g->write_line("");
    if (chunk) {
        g->write("  marched ");
        fmt_u64(nbuf, chunk);
        g->write(nbuf);
        g->write_line(" bytes");
    }
    g->write_line(fail == 0 ? "RESULT: PASS" : "RESULT: FAIL");
}

static int run_mem_ui(void) {
    fos_mem_info_t minfo;
    fos_heap_info_t heap;
    uint64_t usable = 0;
    uint64_t pool = 0;
    int pass = 0;
    int fail = 0;
    uint32_t chunk = 0;

    sti();
    hide_cursor();
    if (g->get_mem_info && g->get_mem_info(&minfo) == 0) {
        usable = minfo.usable_bytes;
    }
    if (g->get_heap_info && g->get_heap_info(&heap) == 0) {
        pool = heap.pool_total;
    }
    draw_mem_frame(0, usable, pool);
    drain_keys();
    for (;;) {
        int d;
        int k = poll_in(&d);
        if (k == IN_NONE) {
            pause_cpu();
            continue;
        }
        if (k == IN_BACK) {
            return IN_BACK;
        }
        if (k == IN_ENTER || k == IN_SPACE) {
            int rc;
            draw_mem_frame(1, usable, pool);
            rc = mem_execute(1, &pass, &fail, &chunk);
            if (rc == -1) {
                draw_mem_frame(0, usable, pool);
                put_str(win_x + 4, win_y + 8, C_HOT, C_WIN, "Cancelled.", 20);
                continue;
            }
            if (rc == -2) {
                draw_mem_frame(0, usable, pool);
                put_str(win_x + 4, win_y + 8, C_HOT, C_WIN, "Need heap (rebuild kernel).", 32);
                continue;
            }
            footer(win_y + win_h - 2, "Enter run again  q back");
            put_str(win_x + 40, win_y + 5, fail ? C_HOT : C_ACC, C_WIN,
                    fail ? "FAIL" : "PASS", 6);
            if (chunk) {
                put_str(win_x + 4, win_y + 9, C_MUTED, C_WIN, "marched", 8);
                put_size(win_x + 14, win_y + 9, C_INFO, C_WIN, chunk);
            }
        }
    }
}

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

static void span_edge(int x0, int y0, int x1, int y1, int *xmin, int *xmax) {
    int dx = iabs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if (y0 >= 0 && y0 < gh && y0 < MAX_ROWS) {
            if (x0 < xmin[y0]) {
                xmin[y0] = x0;
            }
            if (x0 > xmax[y0]) {
                xmax[y0] = x0;
            }
        }
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

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2,
                     uint8_t fg, uint8_t bg, unsigned char ch) {
    int xmin[MAX_ROWS];
    int xmax[MAX_ROWS];
    int y;
    int i;

    for (i = 0; i < gh && i < MAX_ROWS; i++) {
        xmin[i] = gw + 1;
        xmax[i] = -1;
    }
    span_edge(x0, y0, x1, y1, xmin, xmax);
    span_edge(x1, y1, x2, y2, xmin, xmax);
    span_edge(x2, y2, x0, y0, xmin, xmax);
    for (y = 0; y < gh && y < MAX_ROWS; y++) {
        int x;
        if (xmax[y] < xmin[y]) {
            continue;
        }
        if (xmin[y] < 0) {
            xmin[y] = 0;
        }
        if (xmax[y] >= gw) {
            xmax[y] = gw - 1;
        }
        for (x = xmin[y]; x <= xmax[y]; x++) {
            plot(x, y, fg, bg, ch);
        }
    }
}

static void fill_quad(int *x, int *y, uint8_t fg, uint8_t bg, unsigned char ch) {
    fill_tri(x[0], y[0], x[1], y[1], x[2], y[2], fg, bg, ch);
    fill_tri(x[0], y[0], x[2], y[2], x[3], y[3], fg, bg, ch);
}

static void scene_cube(int t) {
    static const int8_t CUBE_V[8][3] = {
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
        {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}
    };
    static const uint8_t CUBE_F[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {1, 5, 6, 2},
        {4, 0, 3, 7}, {0, 4, 5, 1}, {3, 2, 6, 7}
    };
    static const uint8_t FACE_FG[6] = {12, 9, 10, 13, 14, 11};
    static const uint8_t FACE_BG[6] = {4, 1, 2, 5, 6, 3};
    int vx[8], vy[8], vz[8];
    int px[8], py[8];
    int order[6];
    int zavg[6];
    int vis[6];
    int ax = t * 2;
    int ay = t * 3;
    int az = t;
    int scale = (gw < 40 || gh < 16) ? 88 : 132;
    int i, j;
    int gx0, gz;

    gfx_clear(C_WIN);
    /* Ground grid so the cube sits in space. */
    for (gz = 20; gz <= 220; gz += 24) {
        int a0, a1, b0, b1;
        proj3(-180, 92, gz, &a0, &a1);
        proj3(180, 92, gz, &b0, &b1);
        line_to(a0, a1, b0, b1, C_MUTED, C_WIN, CH_DOT);
    }
    for (gx0 = -180; gx0 <= 180; gx0 += 30) {
        int a0, a1, b0, b1;
        proj3(gx0, 92, 20, &a0, &a1);
        proj3(gx0, 92, 220, &b0, &b1);
        line_to(a0, a1, b0, b1, 8, C_WIN, CH_DOT);
    }

    for (i = 0; i < 8; i++) {
        rot3(CUBE_V[i][0] * scale, CUBE_V[i][1] * scale, CUBE_V[i][2] * scale,
             ax, ay, az, &vx[i], &vy[i], &vz[i]);
        vy[i] += isin(t * 2) / 6;
        proj3(vx[i], vy[i], vz[i], &px[i], &py[i]);
    }

    for (i = 0; i < 6; i++) {
        int i0 = CUBE_F[i][0];
        int i1 = CUBE_F[i][1];
        int i2 = CUBE_F[i][2];
        int e1x = vx[i1] - vx[i0];
        int e1y = vy[i1] - vy[i0];
        int e1z = vz[i1] - vz[i0];
        int e2x = vx[i2] - vx[i0];
        int e2y = vy[i2] - vy[i0];
        int e2z = vz[i2] - vz[i0];
        int nx = e1y * e2z - e1z * e2y;
        int ny = e1z * e2x - e1x * e2z;
        int nz = e1x * e2y - e1y * e2x;
        int cx = (vx[i0] + vx[i1] + vx[i2] + vx[CUBE_F[i][3]]) / 4;
        int cy = (vy[i0] + vy[i1] + vy[i2] + vy[CUBE_F[i][3]]) / 4;
        int cz = (vz[i0] + vz[i1] + vz[i2] + vz[CUBE_F[i][3]]) / 4;
        /* Camera sits at (0,0,-280). Visible if the face looks toward it. */
        int to_cam = nx * (0 - cx) + ny * (0 - cy) + nz * (-280 - cz);
        vis[i] = to_cam > 0;
        zavg[i] = cz;
        order[i] = i;
    }
    for (i = 0; i < 5; i++) {
        for (j = i + 1; j < 6; j++) {
            if (zavg[order[j]] > zavg[order[i]]) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    for (i = 0; i < 6; i++) {
        int f = order[i];
        int qx[4];
        int qy[4];
        int light;
        unsigned char ch;
        uint8_t fg;
        uint8_t bg;
        int i0, i1, i2;
        int e1x, e1y, e1z, e2x, e2y, e2z, nx, ny, nz;

        if (!vis[f]) {
            continue;
        }
        for (j = 0; j < 4; j++) {
            int vi = CUBE_F[f][j];
            qx[j] = px[vi];
            qy[j] = py[vi];
        }
        i0 = CUBE_F[f][0];
        i1 = CUBE_F[f][1];
        i2 = CUBE_F[f][2];
        e1x = vx[i1] - vx[i0];
        e1y = vy[i1] - vy[i0];
        e1z = vz[i1] - vz[i0];
        e2x = vx[i2] - vx[i0];
        e2y = vy[i2] - vy[i0];
        e2z = vz[i2] - vz[i0];
        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;
        nx /= 128;
        ny /= 128;
        nz /= 128;
        /* Key light from upper-left, toward the camera. +y is down. */
        light = (-nx - ny - nz) / 180;
        if (light < 0) {
            light = 0;
        }
        if (light > 3) {
            light = 3;
        }
        fg = FACE_FG[f];
        bg = FACE_BG[f];
        if (light <= 0) {
            ch = CH_LITE;
            fg = FACE_BG[f];
            bg = C_WIN;
        } else if (light == 1) {
            ch = CH_DIM;
        } else if (light == 2) {
            ch = CH_MED;
        } else {
            ch = CH_FILL;
            fg = 15;
        }
        fill_quad(qx, qy, fg, bg, ch);
        for (j = 0; j < 4; j++) {
            int a = CUBE_F[f][j];
            int b = CUBE_F[f][(j + 1) & 3];
            line_to(px[a], py[a], px[b], py[b], 15, bg, CH_MED);
        }
    }
}

#define FIRE_MAX (MAX_COLS * MAX_ROWS)
static uint8_t fire[FIRE_MAX];
static uint8_t gfx_scratch[FIRE_MAX];
static int fire_on;
static int16_t rain_y[MAX_COLS];
static uint8_t rain_spd[MAX_COLS];
static int rain_on;
static int life_on;

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

static int gfx_buf_h(void) {
    int h = gh;
    if (gw < 1) {
        return 0;
    }
    if (gw * h > FIRE_MAX) {
        h = FIRE_MAX / gw;
    }
    return h;
}

static void scene_donut(int t) {
    int theta, phi;
    int h = gfx_buf_h();
    int n = gw * h;
    int i;
    int ax = t * 3;
    int ay = t * 2;

    if (h < 4) {
        return;
    }
    for (i = 0; i < n; i++) {
        gfx_scratch[i] = 0;
        fire[i] = 0;
    }
    gfx_clear(C_WIN);
    for (theta = 0; theta < 256; theta += 4) {
        int ct = icos(theta);
        int st = isin(theta);
        for (phi = 0; phi < 256; phi += 3) {
            int cp = icos(phi);
            int sp = isin(phi);
            int r1 = 26;
            int r2 = 64;
            int cx = r2 + (r1 * ct) / 127;
            int x0 = (cx * cp) / 127;
            int y0 = (r1 * st) / 127;
            int z0 = (cx * sp) / 127;
            int xr, yr, zr;
            int sx, sy;
            int zi;
            int idx;
            unsigned char ch;
            uint8_t fg;
            rot3(x0, y0, z0, ax, ay, t, &xr, &yr, &zr);
            proj3(xr * 2, yr * 2, zr * 2, &sx, &sy);
            if (sx < 0 || sy < 0 || sx >= gw || sy >= h) {
                continue;
            }
            zi = (zr + 220) / 3;
            if (zi < 1) {
                zi = 1;
            }
            if (zi > 255) {
                zi = 255;
            }
            idx = sy * gw + sx;
            if ((uint8_t)zi <= gfx_scratch[idx]) {
                continue;
            }
            gfx_scratch[idx] = (uint8_t)zi;
            if (zi > 90) {
                ch = '@';
                fg = 15;
            } else if (zi > 70) {
                ch = '#';
                fg = 14;
            } else if (zi > 50) {
                ch = '*';
                fg = 6;
            } else if (zi > 30) {
                ch = '+';
                fg = 6;
            } else {
                ch = '.';
                fg = 8;
            }
            plot(sx, sy, fg, C_WIN, ch);
        }
    }
}

static void scene_tunnel(int t) {
    int y, x;
    int cx = gw / 2;
    int cy = gh / 2;
    for (y = 0; y < gh; y++) {
        for (x = 0; x < gw; x++) {
            int dx = x - cx;
            int dy = (y - cy) * 2;
            int r = isqrt(dx * dx + dy * dy);
            int ang;
            int u, v, idx;
            uint8_t fg;
            unsigned char ch;
            if (r < 1) {
                r = 1;
            }
            ang = (dx * 48) / r + t;
            u = (ang + t) & 15;
            v = ((200 / r) + t) & 15;
            idx = u ^ v;
            if (idx < 4) {
                fg = 1;
                ch = CH_LITE;
            } else if (idx < 8) {
                fg = 9;
                ch = CH_DIM;
            } else if (idx < 12) {
                fg = 11;
                ch = CH_MED;
            } else {
                fg = 15;
                ch = CH_FILL;
            }
            plot(x, y, fg, C_WIN, ch);
        }
    }
}

static void rain_init(void) {
    int x;
    for (x = 0; x < MAX_COLS; x++) {
        rain_y[x] = (int16_t)(rnd() % 40u);
        rain_spd[x] = (uint8_t)(1 + (rnd() % 3u));
    }
    rain_on = 1;
}

static void scene_matrix(int t) {
    int x, k;
    (void)t;
    gfx_clear(C_WIN);
    for (x = 0; x < gw; x++) {
        rain_y[x] = (int16_t)(rain_y[x] + rain_spd[x]);
        if (rain_y[x] > gh + 12) {
            rain_y[x] = (int16_t)(-((int)(rnd() % 12u)));
            rain_spd[x] = (uint8_t)(1 + (rnd() % 3u));
        }
        for (k = 0; k < 10; k++) {
            int y = rain_y[x] - k;
            unsigned char ch;
            uint8_t fg;
            if (y < 0 || y >= gh) {
                continue;
            }
            ch = (unsigned char)(33 + (rnd() % 90u));
            if (k == 0) {
                fg = 15;
            } else if (k < 3) {
                fg = 10;
            } else if (k < 6) {
                fg = 2;
            } else {
                fg = 8;
            }
            plot(x, y, fg, C_WIN, ch);
        }
    }
}

static void life_init(void) {
    int n = gw * gfx_buf_h();
    int i;
    for (i = 0; i < n && i < FIRE_MAX; i++) {
        fire[i] = (uint8_t)((rnd() % 5u) == 0);
    }
    life_on = 1;
}

static void scene_life(int t) {
    int w = gw;
    int h = gfx_buf_h();
    int x, y;
    (void)t;
    if (w < 4 || h < 4) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int n = 0;
            int dy, dx;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int xx, yy;
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    xx = x + dx;
                    yy = y + dy;
                    if (xx < 0) {
                        xx = w - 1;
                    } else if (xx >= w) {
                        xx = 0;
                    }
                    if (yy < 0) {
                        yy = h - 1;
                    } else if (yy >= h) {
                        yy = 0;
                    }
                    n += fire[yy * w + xx] ? 1 : 0;
                }
            }
            {
                int alive = fire[y * w + x] ? 1 : 0;
                gfx_scratch[y * w + x] = (uint8_t)((alive && (n == 2 || n == 3)) ||
                                                   (!alive && n == 3));
            }
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t a = gfx_scratch[y * w + x];
            fire[y * w + x] = a;
            if (a) {
                plot(x, y, 10, C_WIN, CH_FILL);
            } else {
                plot(x, y, C_MUTED, C_WIN, ' ');
            }
        }
    }
}

static const char *GFX_NAME[N_GFX] = {
    "Plasma", "Starfield", "3D bounce", "Fire",
    "Donut", "Tunnel", "Matrix", "Life", "Cube"
};

static void draw_gfx_chrome(int scene) {
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " GFX  ·  Graphics demo ");
    put_str(win_x + 3, win_y + 1, C_TITLE, C_BTN_BG, GFX_NAME[scene], 16);
    footer(win_y + win_h - 2, "1-9 / Left Right  scene   Space pause   q back");
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
    rain_on = 0;
    life_on = 0;
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
            stars_on = fire_on = bounce_on = rain_on = life_on = 0;
            draw_gfx_chrome(scene);
        } else if (k == IN_RIGHT) {
            scene = (scene + 1) % N_GFX;
            stars_on = fire_on = bounce_on = rain_on = life_on = 0;
            draw_gfx_chrome(scene);
        } else if (k == IN_DIGIT && d >= 0 && d < N_GFX) {
            scene = d;
            stars_on = fire_on = bounce_on = rain_on = life_on = 0;
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
        } else if (scene == 3) {
            if (!fire_on) {
                fire_init();
            }
            scene_fire(t);
        } else if (scene == 4) {
            scene_donut(t);
        } else if (scene == 5) {
            scene_tunnel(t);
        } else if (scene == 6) {
            if (!rain_on) {
                rain_init();
            }
            scene_matrix(t);
        } else if (scene == 7) {
            if (!life_on) {
                life_init();
            }
            scene_life(t);
        } else {
            scene_cube(t);
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
            g->show_error("Sound API missing - rebuild the kernel");
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

/* ---- hardware monitor ---- */

typedef struct {
    char vendor[13];
    char brand[49];
    char hv[13];
    char feats[48];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t threads;
    int has_hv;
} hw_cpu_t;

static void hw_cpuid(uint32_t leaf, uint32_t sub, uint32_t *eax, uint32_t *ebx,
                     uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(sub));
}

static uint64_t hw_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void hw_idle_tick(void) {
    if (g->sleep_ms) {
        g->sleep_ms(1);
    } else {
        __asm__ volatile("sti; hlt");
    }
}

static void hw_feat_add(char *out, int cap, const char *tag, int on) {
    int n, t, i;
    if (!on || cap <= 1) {
        return;
    }
    n = 0;
    while (out[n]) {
        n++;
    }
    t = 0;
    while (tag[t]) {
        t++;
    }
    if (n + 1 + t >= cap) {
        return;
    }
    if (n) {
        out[n++] = ' ';
    }
    for (i = 0; i < t; i++) {
        out[n++] = tag[i];
    }
    out[n] = 0;
}

static void hw_identify(hw_cpu_t *cpu) {
    uint32_t eax, ebx, ecx, edx;
    uint32_t max_ext;
    uint32_t *dest;
    uint32_t base_fam, base_mod;
    char *p;

    cpu->vendor[0] = 0;
    cpu->brand[0] = 0;
    cpu->hv[0] = 0;
    cpu->feats[0] = 0;
    cpu->family = 0;
    cpu->model = 0;
    cpu->stepping = 0;
    cpu->threads = 1;
    cpu->has_hv = 0;

    hw_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    *(uint32_t *)(cpu->vendor + 0) = ebx;
    *(uint32_t *)(cpu->vendor + 4) = edx;
    *(uint32_t *)(cpu->vendor + 8) = ecx;
    cpu->vendor[12] = 0;

    hw_cpuid(0x80000000, 0, &max_ext, &ebx, &ecx, &edx);
    if (max_ext >= 0x80000004) {
        dest = (uint32_t *)cpu->brand;
        hw_cpuid(0x80000002, 0, &eax, &ebx, &ecx, &edx);
        dest[0] = eax;
        dest[1] = ebx;
        dest[2] = ecx;
        dest[3] = edx;
        hw_cpuid(0x80000003, 0, &eax, &ebx, &ecx, &edx);
        dest[4] = eax;
        dest[5] = ebx;
        dest[6] = ecx;
        dest[7] = edx;
        hw_cpuid(0x80000004, 0, &eax, &ebx, &ecx, &edx);
        dest[8] = eax;
        dest[9] = ebx;
        dest[10] = ecx;
        dest[11] = edx;
        cpu->brand[48] = 0;
        p = cpu->brand;
        while (*p == ' ') {
            p++;
        }
        if (p != cpu->brand) {
            int i = 0;
            while (p[i] && i < 48) {
                cpu->brand[i] = p[i];
                i++;
            }
            cpu->brand[i] = 0;
        }
    }

    hw_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    base_fam = (eax >> 8) & 0xFu;
    base_mod = (eax >> 4) & 0xFu;
    cpu->family = base_fam;
    if (base_fam == 0xFu) {
        cpu->family += (eax >> 20) & 0xFFu;
    }
    cpu->model = base_mod;
    if (base_fam == 0x6u || base_fam == 0xFu) {
        cpu->model |= ((eax >> 16) & 0xFu) << 4;
    }
    cpu->stepping = eax & 0xFu;
    cpu->threads = (ebx >> 16) & 0xFFu;
    if (cpu->threads == 0) {
        cpu->threads = 1;
    }

    hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "TSC", (edx >> 4) & 1);
    hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "SSE", (edx >> 25) & 1);
    hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "SSE2", (edx >> 26) & 1);
    hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "AVX", (ecx >> 28) & 1);
    if (max_ext >= 0x80000001) {
        uint32_t eeax, eebx, eecx, eedx;
        hw_cpuid(0x80000001, 0, &eeax, &eebx, &eecx, &eedx);
        hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "LM", (eedx >> 29) & 1);
    }
    if (ecx & (1u << 31)) {
        cpu->has_hv = 1;
        hw_cpuid(0x40000000, 0, &eax, &ebx, &ecx, &edx);
        *(uint32_t *)(cpu->hv + 0) = ebx;
        *(uint32_t *)(cpu->hv + 4) = ecx;
        *(uint32_t *)(cpu->hv + 8) = edx;
        cpu->hv[12] = 0;
        hw_feat_add(cpu->feats, (int)sizeof(cpu->feats), "HV", 1);
    }
}

static uint32_t hw_calibrate_mhz(void) {
    uint64_t t0, t1, dt, c0, c1;
    if (!g->sleep_ms || !g->get_ticks_ms) {
        return 0;
    }
    t0 = now_ms();
    c0 = hw_rdtsc();
    g->sleep_ms(100);
    c1 = hw_rdtsc();
    t1 = now_ms();
    dt = t1 - t0;
    if (dt == 0) {
        dt = 1;
    }
    return (uint32_t)((c1 - c0) / dt / 1000ull);
}

static uint32_t hw_pct(uint64_t num, uint64_t den) {
    if (den == 0) {
        return 0;
    }
    if (num >= den) {
        return 100;
    }
    return (uint32_t)((num * 100ull) / den);
}

static uint8_t hw_pct_fg(uint32_t pct) {
    if (pct >= 90) {
        return C_HOT;
    }
    if (pct >= 60) {
        return C_TITLE;
    }
    return C_ACC;
}

static int hw_fmt_size(char *out, uint64_t bytes) {
    const char *unit = "B";
    uint64_t v = bytes;
    int n;
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        v = bytes / (1024ull * 1024ull * 1024ull);
        unit = "GB";
    } else if (bytes >= 1024ull * 1024ull) {
        v = bytes / (1024ull * 1024ull);
        unit = "MB";
    } else if (bytes >= 1024ull) {
        v = bytes / 1024ull;
        unit = "KB";
    }
    n = fmt_u64(out, v);
    out[n++] = ' ';
    out[n++] = unit[0];
    if (unit[1]) {
        out[n++] = unit[1];
    }
    out[n] = 0;
    return n;
}

static void hw_put_pct(int x, int y, uint8_t fg, uint32_t pct) {
    char buf[8];
    int n = 0;
    if (pct > 100) {
        pct = 100;
    }
    if (pct >= 100) {
        buf[n++] = '1';
        buf[n++] = '0';
        buf[n++] = '0';
    } else {
        if (pct >= 10) {
            buf[n++] = (char)('0' + pct / 10);
        } else {
            buf[n++] = ' ';
        }
        buf[n++] = (char)('0' + (pct % 10));
    }
    buf[n++] = '%';
    buf[n] = 0;
    put_str(x, y, fg, C_WIN, buf, 4);
}

static void hw_put_freq(int x, int y, uint32_t mhz) {
    char buf[24];
    int n;
    if (mhz == 0) {
        put_str(x, y, C_MUTED, C_WIN, "? MHz", 8);
        return;
    }
    if (mhz >= 1000u) {
        uint32_t g = mhz / 1000u;
        uint32_t frac = (mhz % 1000u) / 100u;
        n = fmt_u64(buf, g);
        buf[n++] = '.';
        buf[n++] = (char)('0' + frac);
        buf[n++] = ' ';
        buf[n++] = 'G';
        buf[n++] = 'H';
        buf[n++] = 'z';
        buf[n] = 0;
    } else {
        n = fmt_u64(buf, mhz);
        buf[n++] = ' ';
        buf[n++] = 'M';
        buf[n++] = 'H';
        buf[n++] = 'z';
        buf[n] = 0;
    }
    put_str(x, y, C_ACC, C_WIN, buf, 23);
}

static void hw_fmt_clock(char *out, const fos_rtc_t *t) {
    uint32_t y = t->year;
    out[0] = (char)('0' + (y / 1000u) % 10u);
    out[1] = (char)('0' + (y / 100u) % 10u);
    out[2] = (char)('0' + (y / 10u) % 10u);
    out[3] = (char)('0' + y % 10u);
    out[4] = '-';
    out[5] = (char)('0' + t->month / 10u);
    out[6] = (char)('0' + t->month % 10u);
    out[7] = '-';
    out[8] = (char)('0' + t->day / 10u);
    out[9] = (char)('0' + t->day % 10u);
    out[10] = ' ';
    out[11] = (char)('0' + t->hour / 10u);
    out[12] = (char)('0' + t->hour % 10u);
    out[13] = ':';
    out[14] = (char)('0' + t->minute / 10u);
    out[15] = (char)('0' + t->minute % 10u);
    out[16] = ':';
    out[17] = (char)('0' + t->second / 10u);
    out[18] = (char)('0' + t->second % 10u);
    out[19] = 0;
}

static void hw_fmt_uptime(char *out, uint64_t ms) {
    uint64_t s = ms / 1000ull;
    uint64_t h = s / 3600ull;
    uint32_t m = (uint32_t)((s / 60ull) % 60ull);
    uint32_t sec = (uint32_t)(s % 60ull);
    int n = fmt_u64(out, h);
    out[n++] = ':';
    out[n++] = (char)('0' + m / 10u);
    out[n++] = (char)('0' + m % 10u);
    out[n++] = ':';
    out[n++] = (char)('0' + sec / 10u);
    out[n++] = (char)('0' + sec % 10u);
    out[n] = 0;
}

static int hw_bar_x;
static int hw_bar_w;
static int hw_y_cpu;
static int hw_y_hist;
static int hw_y_ram;
static int hw_y_heap;
static int hw_y_load;
static int hw_y_clk;
static int hw_y_io;
static int hw_got_mouse;
static int hw_got_sb;
static int hw_n_disks;

static void hw_meter(int y, const char *name, uint32_t pct, uint8_t bar_fg,
                     const char *tail) {
    int name_w = 4;
    fill_span(win_x + 2, y, win_w - 4, C_FG, C_WIN, ' ');
    put_str(win_x + 3, y, C_TITLE, C_WIN, name, name_w);
    bar(hw_bar_x, y, hw_bar_w, pct, 100, bar_fg, C_WIN);
    hw_put_pct(hw_bar_x + hw_bar_w + 1, y, bar_fg, pct);
    if (tail && tail[0]) {
        put_str(hw_bar_x + hw_bar_w + 6, y, C_MUTED, C_WIN, tail, win_w - 20);
    }
}

static void hw_draw_hist(const uint8_t *hist, int n, int head) {
    int w = hw_bar_w - 2;
    int i;
    fill_span(win_x + 2, hw_y_hist, win_w - 4, C_FG, C_WIN, ' ');
    if (w < 4) {
        return;
    }
    if (w > HW_HIST) {
        w = HW_HIST;
    }
    for (i = 0; i < w; i++) {
        uint8_t v = 0;
        unsigned char ch;
        uint8_t fg;
        int idx;
        if (n <= 0) {
            put_xy(hw_bar_x + 1 + i, hw_y_hist, C_MUTED, C_WIN, CH_DOT);
            continue;
        }
        if (n < HW_HIST) {
            idx = i - (w - n);
            if (idx < 0 || idx >= n) {
                put_xy(hw_bar_x + 1 + i, hw_y_hist, C_MUTED, C_WIN, CH_DOT);
                continue;
            }
            v = hist[idx];
        } else {
            idx = (head + HW_HIST - w + i) % HW_HIST;
            v = hist[idx];
        }
        if (v < 12) {
            ch = CH_DOT;
            fg = C_MUTED;
        } else if (v < 37) {
            ch = CH_LITE;
            fg = C_ACC;
        } else if (v < 62) {
            ch = CH_DIM;
            fg = C_ACC;
        } else if (v < 87) {
            ch = CH_MED;
            fg = C_TITLE;
        } else {
            ch = CH_FILL;
            fg = C_HOT;
        }
        put_xy(hw_bar_x + 1 + i, hw_y_hist, fg, C_WIN, ch);
    }
}

static void hw_draw_chrome(const hw_cpu_t *cpu, uint32_t mhz) {
    char line[80];
    int n;
    int y;
    int inner;

    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " HW  ·  Hardware monitor ");
    y = win_y + 3;
    put_str(win_x + 3, y, C_FG, C_WIN,
            cpu->brand[0] ? cpu->brand : cpu->vendor, win_w - 6);
    y++;
    hw_put_freq(win_x + 3, y, mhz);
    put_str(win_x + 16, y, C_MUTED, C_WIN, "threads", 8);
    put_u64(win_x + 24, y, C_FG, C_WIN, cpu->threads);
    put_str(win_x + 30, y, C_MUTED, C_WIN, "fam/mod", 8);
    n = fmt_u64(line, cpu->family);
    line[n++] = '/';
    n += fmt_u64(line + n, cpu->model);
    line[n++] = '.';
    n += fmt_u64(line + n, cpu->stepping);
    line[n] = 0;
    put_str(win_x + 39, y, C_INFO, C_WIN, line, 20);
    y++;
    if (cpu->feats[0]) {
        put_str(win_x + 3, y, C_MUTED, C_WIN, cpu->feats, win_w - 6);
    }
    if (cpu->has_hv && cpu->hv[0]) {
        int fl = 0;
        while (cpu->feats[fl]) {
            fl++;
        }
        put_str(win_x + 4 + fl, y, C_INFO, C_WIN, cpu->hv, 12);
    }
    y++;
    fill_span(win_x + 3, y, win_w - 6, C_FRAME, C_WIN, CH_H);
    y++;

    hw_bar_x = win_x + 8;
    hw_bar_w = win_w - 28;
    if (hw_bar_w > 42) {
        hw_bar_w = 42;
    }
    if (hw_bar_w < 12) {
        hw_bar_w = win_w > 16 ? win_w - 14 : 10;
        hw_bar_x = win_x + 8;
    }

    hw_y_cpu = y;
    y += 2;
    hw_y_hist = hw_y_cpu + 1;
    hw_y_ram = y;
    y++;
    hw_y_heap = y;
    y++;
    hw_y_load = y;
    y += 2;
    fill_span(win_x + 3, y - 1, win_w - 6, C_FRAME, C_WIN, CH_H);
    hw_y_clk = y;
    y++;
    hw_y_io = y;

    inner = win_y + win_h - 2;
    if (hw_y_io >= inner) {
        hw_y_io = inner - 1;
    }
    footer(inner, "Space load on/off  Left/Right 0-9  q back");
}

static void hw_update(uint32_t cpu_pct, int load, const uint8_t *hist, int hist_n,
                      int hist_head) {
    fos_mem_info_t minfo;
    fos_heap_info_t heap;
    fos_rtc_t rtc;
    char tail[48];
    char tmp[32];
    uint64_t ram_used = 0;
    uint64_t ram_total = 0;
    uint32_t ram_pct = 0;
    uint32_t heap_pct = 0;
    int n;
    int x;

    minfo.usable_bytes = 0;
    minfo.kernel_size = 0;
    heap.pool_total = 0;
    heap.pool_used = 0;
    heap.heap_reserved = 0;
    heap.heap_used = 0;
    heap.heap_blocks = 0;
    if (g->get_mem_info) {
        (void)g->get_mem_info(&minfo);
    }
    if (g->get_heap_info) {
        (void)g->get_heap_info(&heap);
    }

    ram_total = minfo.usable_bytes;
    ram_used = minfo.kernel_size + heap.pool_used;
    if (ram_used > ram_total && ram_total) {
        ram_used = ram_total;
    }
    ram_pct = hw_pct(ram_used, ram_total);
    heap_pct = hw_pct(heap.heap_used, heap.heap_reserved ? heap.heap_reserved
                                                         : heap.pool_total);

    hw_meter(hw_y_cpu, "CPU", cpu_pct, hw_pct_fg(cpu_pct), NULL);
    hw_draw_hist(hist, hist_n, hist_head);

    n = hw_fmt_size(tail, ram_used);
    tail[n++] = ' ';
    tail[n++] = '/';
    tail[n++] = ' ';
    n += hw_fmt_size(tail + n, ram_total);
    tail[n] = 0;
    hw_meter(hw_y_ram, "RAM", ram_pct, C_INFO, tail);

    n = hw_fmt_size(tail, heap.heap_used);
    tail[n++] = ' ';
    tail[n++] = '/';
    tail[n++] = ' ';
    n += hw_fmt_size(tail + n, heap.heap_reserved);
    tail[n] = 0;
    hw_meter(hw_y_heap, "HEAP", heap_pct, C_ACC, tail);

    hw_meter(hw_y_load, "LOAD", (uint32_t)load, C_FRAME,
             load ? "synth" : "idle");

    fill_span(win_x + 2, hw_y_clk, win_w - 4, C_FG, C_WIN, ' ');
    put_str(win_x + 3, hw_y_clk, C_TITLE, C_WIN, "CLK", 4);
    if (g->rtc_read && g->rtc_read(&rtc) == 0 && rtc.year >= 1970) {
        hw_fmt_clock(tmp, &rtc);
        put_str(win_x + 8, hw_y_clk, C_FG, C_WIN, tmp, 20);
    } else {
        put_str(win_x + 8, hw_y_clk, C_MUTED, C_WIN, "(no RTC)", 10);
    }
    put_str(win_x + 30, hw_y_clk, C_MUTED, C_WIN, "up", 3);
    hw_fmt_uptime(tmp, now_ms());
    put_str(win_x + 33, hw_y_clk, C_INFO, C_WIN, tmp, 16);

    fill_span(win_x + 2, hw_y_io, win_w - 4, C_FG, C_WIN, ' ');
    put_str(win_x + 3, hw_y_io, C_TITLE, C_WIN, "I/O", 4);
    x = win_x + 8;
    if (hw_got_sb) {
        put_str(x, hw_y_io, C_ACC, C_WIN, "SB16", 4);
        x += 6;
    } else {
        put_str(x, hw_y_io, C_MUTED, C_WIN, "no SB", 5);
        x += 7;
    }
    put_str(x, hw_y_io, hw_got_mouse ? C_ACC : C_MUTED, C_WIN,
            hw_got_mouse ? "mouse" : "no mouse", 8);
    x += hw_got_mouse ? 7 : 10;
    put_u64(x, hw_y_io, C_FG, C_WIN, (uint64_t)hw_n_disks);
    x += fmt_u64(tmp, (uint64_t)hw_n_disks);
    put_str(x, hw_y_io, C_MUTED, C_WIN, hw_n_disks == 1 ? " disk  " : " disks ", 8);
    x += 7;
    put_u64(x, hw_y_io, C_FG, C_WIN, (uint64_t)cols);
    x += fmt_u64(tmp, (uint64_t)cols);
    put_str(x, hw_y_io, C_MUTED, C_WIN, "x", 1);
    put_u64(x + 1, hw_y_io, C_FG, C_WIN, (uint64_t)rows);
}

static void hw_hist_push(uint8_t *hist, int *n, int *head, uint8_t v) {
    if (*n < HW_HIST) {
        hist[*n] = v;
        (*n)++;
        return;
    }
    hist[*head] = v;
    *head = (*head + 1) % HW_HIST;
}

static int hw_apply_key(int k, int d, int *load, int *last_load) {
    if (k == IN_BACK) {
        return -1;
    }
    if (k == IN_SPACE || k == IN_ENTER) {
        if (*load == 0) {
            *load = *last_load ? *last_load : 100;
        } else {
            *last_load = *load;
            *load = 0;
        }
        return 0;
    }
    if (k == IN_LEFT || k == IN_DOWN) {
        *load = *load >= 10 ? *load - 10 : 0;
        if (*load) {
            *last_load = *load;
        }
        return 0;
    }
    if (k == IN_RIGHT || k == IN_UP) {
        *load = *load <= 90 ? *load + 10 : 100;
        *last_load = *load;
        return 0;
    }
    if (k == IN_DIGIT) {
        if (d == 9) {
            *load = 0;
        } else if (d >= 0 && d <= 8) {
            *load = (d + 1) * 10;
            *last_load = *load;
        }
    }
    return 0;
}

static void hw_burn_until(uint64_t until, int *key, int *digit) {
    uint64_t acc = (uint64_t)rng_state ^ 0x9E3779B97F4A7C15ULL;
    uint32_t n = 0;

    *key = IN_NONE;
    if (digit) {
        *digit = -1;
    }
    while (now_ms() < until) {
        uint32_t k;
        for (k = 0; k < 256; k++) {
            acc *= 6364136223846793005ULL;
            acc += 1;
            acc ^= acc >> 17;
        }
        n++;
        if ((n & 7u) == 0) {
            int d;
            int kk = poll_in(&d);
            if (kk != IN_NONE) {
                *key = kk;
                if (digit) {
                    *digit = d;
                }
                rng_state = (uint32_t)acc;
                return;
            }
        }
    }
    rng_state = (uint32_t)acc;
}

static int hw_load_hit(int mx, int my, int *load) {
    int inner;
    if (my != hw_y_load) {
        return 0;
    }
    inner = hw_bar_w - 2;
    if (inner < 1 || mx < hw_bar_x || mx >= hw_bar_x + hw_bar_w) {
        return 0;
    }
    if (mx <= hw_bar_x) {
        *load = 0;
    } else if (mx >= hw_bar_x + hw_bar_w - 1) {
        *load = 100;
    } else {
        *load = ((mx - hw_bar_x - 1) * 100) / inner;
        if (*load > 100) {
            *load = 100;
        }
        if (*load < 0) {
            *load = 0;
        }
    }
    return 1;
}

static int hw_poll_load(int *load, int *last_load) {
    int mx, my;
    if (!mouse_click(&mx, &my)) {
        return 0;
    }
    if (hw_load_hit(mx, my, load)) {
        if (*load) {
            *last_load = *load;
        }
        return 1;
    }
    return 0;
}

static int run_hw_ui(void) {
    hw_cpu_t cpu;
    uint32_t mhz;
    int load = 0;
    int last_load = 100;
    uint8_t hist[HW_HIST];
    int hist_n = 0;
    int hist_head = 0;
    uint32_t cpu_pct = 0;
    fos_mouse_t mouse;

    sti();
    hide_cursor();
    layout_full();
    desktop();
    draw_window(win_x, win_y, win_w, win_h, " HW  ·  Hardware monitor ");
    put_str(win_x + 3, win_y + 3, C_MUTED, C_WIN, "Calibrating TSC against PIT...", 36);
    footer(win_y + win_h - 2, "q back");
    hw_identify(&cpu);
    mhz = hw_calibrate_mhz();
    hw_got_sb = g->sound_present && g->sound_present();
    hw_got_mouse = g->mouse_poll && g->mouse_poll(&mouse);
    hw_n_disks = g->drive_count ? g->drive_count() : 0;
    hw_draw_chrome(&cpu, mhz);
    hw_update(0, load, hist, 0, 0);
    drain_keys();

    for (;;) {
        uint64_t t0 = now_ms();
        int d = -1;
        int k = IN_NONE;
        uint32_t busy;
        uint32_t wall;

        (void)hw_poll_load(&load, &last_load);
        k = poll_in(&d);
        if (k != IN_NONE) {
            if (hw_apply_key(k, d, &load, &last_load) < 0) {
                return IN_BACK;
            }
            k = IN_NONE;
        }

        if (load > 0) {
            uint64_t burn_ms = ((uint64_t)HW_FRAME_MS * (uint64_t)load) / 100ull;
            hw_burn_until(t0 + burn_ms, &k, &d);
            if (k != IN_NONE) {
                if (hw_apply_key(k, d, &load, &last_load) < 0) {
                    return IN_BACK;
                }
                k = IN_NONE;
            }
        }

        hw_update(cpu_pct, load, hist, hist_n, hist_head);
        busy = (uint32_t)(now_ms() - t0);

        while (now_ms() - t0 < (uint64_t)HW_FRAME_MS) {
            (void)hw_poll_load(&load, &last_load);
            d = -1;
            k = poll_in(&d);
            if (k != IN_NONE) {
                break;
            }
            if (g->has_key) {
                (void)g->has_key();
            }
            hw_idle_tick();
        }

        wall = (uint32_t)(now_ms() - t0);
        if (wall == 0) {
            wall = 1;
        }
        cpu_pct = (busy * 100u) / wall;
        if (cpu_pct > 100) {
            cpu_pct = 100;
        }
        hw_hist_push(hist, &hist_n, &hist_head, (uint8_t)cpu_pct);

        if (k != IN_NONE) {
            if (hw_apply_key(k, d, &load, &last_load) < 0) {
                return IN_BACK;
            }
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
    put_str(win_x + 4, win_y + 8, C_INFO, C_WIN, "Full show - 100k sieve first...", 36);
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
        while (now_ms() - t0 < 900u) {
            int d;
            if (poll_in(&d) == IN_BACK) {
                return IN_BACK;
            }
            pause_cpu();
        }
    }
    {
        fos_mem_info_t minfo;
        fos_heap_info_t heap;
        uint64_t usable = 0;
        uint64_t pool = 0;
        int pass = 0;
        int fail = 0;
        uint32_t chunk = 0;
        int mrc;
        if (g->get_mem_info && g->get_mem_info(&minfo) == 0) {
            usable = minfo.usable_bytes;
        }
        if (g->get_heap_info && g->get_heap_info(&heap) == 0) {
            pool = heap.pool_total;
        }
        draw_mem_frame(1, usable, pool);
        mrc = mem_execute(1, &pass, &fail, &chunk);
        if (mrc == -1) {
            return IN_BACK;
        }
        (void)chunk;
        {
            uint64_t t0 = now_ms();
            while (now_ms() - t0 < 900u) {
                int d;
                if (poll_in(&d) == IN_BACK) {
                    return IN_BACK;
                }
                pause_cpu();
            }
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
    put_str(win_x + 4, win_y + 6, C_FG, C_WIN, "Memory, graphics, and audio played.", 40);
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
    {'2', "BURN", "One-minute soak", "Keep the CPU busy for 60 seconds"},
    {'3', "RAM", "Memory test", "Heap stress and marching RAM patterns"},
    {'4', "GFX", "Graphics demo", "Plasma, stars, 3D, cube, fire, donut, tunnel, life"},
    {'5', "SND", "Audio demo", "Scale, Ode to Joy, dual-voice synth"},
    {'6', "HW", "Hardware monitor", "Live CPU %, RAM, heap, clock, and load"},
    {'7', "ALL", "Run everything", "CPU, RAM, each graphics scene, then sound"},
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
    footer(win_y + win_h - 2, "Arrows / mouse  Enter  1-7  q quit");
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
    if (arg[0] && (arg_is(arg, "primes") || arg_is(arg, "cpu") ||
                   arg_is(arg, "prime"))) {
        cpu_print_stdout(0);
        return;
    }
    if (arg[0] && (arg_is(arg, "mem") || arg_is(arg, "memory") ||
                   arg_is(arg, "ram"))) {
        mem_print_stdout();
        return;
    }
    if (arg[0] && (arg_is(arg, "burn") || arg_is(arg, "soak"))) {
        burn_print_stdout(BURN_MS);
        return;
    }
    if (arg[0] && (arg_is(arg, "gfx") || arg_is(arg, "graphics"))) {
        start = 3;
    } else if (arg[0] && (arg_is(arg, "snd") || arg_is(arg, "audio") ||
                          arg_is(arg, "sound"))) {
        start = 4;
    } else if (arg[0] && (arg_is(arg, "hw") || arg_is(arg, "mon") ||
                          arg_is(arg, "monitor") || arg_is(arg, "top"))) {
        start = 5;
    } else if (arg[0] && arg_is(arg, "all")) {
        start = 6;
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

    if (start == 3) {
        (void)run_gfx_ui(8, 0);
        restore_console();
        return;
    }
    if (start == 4) {
        (void)run_snd_ui(-1);
        restore_console();
        return;
    }
    if (start == 5) {
        (void)run_hw_ui();
        restore_console();
        return;
    }
    if (start == 6) {
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
        } else if (k == IN_DIGIT && d >= 0 && d < N_MENU - 1) {
            sel = d;
            k = IN_ENTER;
        }
        if (k == IN_ENTER || k == IN_SPACE) {
            if (sel == N_MENU - 1) {
                break;
            }
            if (sel == 0) {
                (void)run_cpu_ui(3);
            } else if (sel == 1) {
                (void)run_burn_ui(BURN_MS);
            } else if (sel == 2) {
                (void)run_mem_ui();
            } else if (sel == 3) {
                (void)run_gfx_ui(8, 0);
            } else if (sel == 4) {
                (void)run_snd_ui(-1);
            } else if (sel == 5) {
                (void)run_hw_ui();
            } else if (sel == 6) {
                (void)run_all();
            }
            draw_menu(sel);
            drain_keys();
        }
    }
    restore_console();
}
