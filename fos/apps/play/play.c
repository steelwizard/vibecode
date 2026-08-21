/*
 * play.c — Stream WAV / MP3 through the Sound Blaster.
 *
 *   play FILE.WAV
 *   play FILE.MP3
 *   play FILE.MID
 *
 * Full-screen TUI: purple desktop, grey player, blue title bar.
 * ← → seek 5s, ↑ ↓ 30s, Home / End, q / Esc / Ctrl+C quit.
 */

#include "fos_api.h"
#include "mp3dec_fos.h"
#include "midi_fos.h"

#define IN_MAX     4096
#define PCM8_MAX   4096
#define RING_CAP   (256u * 1024u)
#define FILE_CAP   (64u * 1024u)

#define BG_DESK    5  /* purple */
#define FG_DESK    13
#define BG_SHADOW  0
#define BG_WIN     7  /* light grey */
#define FG_WIN     0  /* black */
#define FG_MUTED   8
#define FG_TITLE   15 /* white on blue, like PAINT */
#define BG_TITLE   1
#define FG_BAR     5
#define FG_EMPTY   8
#define FG_ERR     4
#define FG_OK      2

#define CH_TL      0xC9u /* ╔ */
#define CH_TR      0xBBu /* ╗ */
#define CH_BL      0xC8u /* ╚ */
#define CH_BR      0xBCu /* ╝ */
#define CH_H       0xCDu /* ═ */
#define CH_V       0xBAu /* ║ */
#define CH_FILL    0xDBu /* █ */
#define CH_EMPTY   0xB0u /* ░ */
#define CH_NOTE    0x0Eu /* ♫ */

#define WIN_W      64
#define WIN_H      13
#define BAR_W      (WIN_W - 8)

#define PLAY_OK    0
#define PLAY_QUIT  1
#define PLAY_SEEK  2
#define PLAY_ERR   (-1)

#define SEEK_STEP_MS   5000u
#define SEEK_JUMP_MS   30000u
#define SEEK_PAGE_MS   60000u
#define SEEK_TAIL      0xFFFFFFFFu

#define CH_LEFT    0x1Bu /* ← */
#define CH_RIGHT   0x1Au /* → */
#define CH_UP      0x18u /* ↑ */
#define CH_DOWN    0x19u /* ↓ */

/* Window origin, centred on the real console at startup. */
static int win_x = (80 - WIN_W) / 2;
static int win_y = 5;

static void init_geometry(fos_api_t *api) {
    int c = 80;
    int r = 25;

    if (api->get_term_size) {
        api->get_term_size(&c, &r);
    }
    win_x = (c - WIN_W) / 2;
    win_y = (r - WIN_H) / 2;
    if (win_x < 0) {
        win_x = 0;
    }
    if (win_y < 0) {
        win_y = 0;
    }
}

static uint8_t inbuf_fallback[IN_MAX];
static uint8_t pcm_fallback[PCM8_MAX];
static uint8_t chunk_fallback[PCM8_MAX];
static int16_t mp3pcm[FOS_MP3_MAX_SAMPLES];

static uint8_t *filebuf;
static uint32_t file_cap;
static uint8_t *ring;
static uint32_t ring_cap;
static uint32_t ring_r, ring_w, ring_n;
static uint8_t *chunkbuf;
static uint32_t dma_chunk;
static int prefilled;
static int have_seek;
static uint32_t seek_to_ms;

typedef struct {
    const char *path;
    const char *kind;
    const char *status;
    uint8_t     status_fg;
    uint32_t    rate;
    uint16_t    ch;
    uint16_t    bits;
    uint32_t    pos;
    uint32_t    total;
    uint32_t    pos_ms;
    uint32_t    total_ms;
    uint32_t    live_base_ms;
    uint32_t    live_chunk_ms;
    uint64_t    live_start;
    int         live;
    int         painted;
    int         last_filled;
    uint32_t    last_sec;
} play_ui_t;

static play_ui_t ui;

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t cstr_len(const char *s) {
    size_t n = 0;
    while (s && s[n]) {
        n++;
    }
    return n;
}

static const char *base_name(const char *path) {
    const char *b = path;
    const char *p;
    if (!path || !path[0]) {
        return "";
    }
    for (p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            b = p + 1;
        }
    }
    return *b ? b : path;
}

static int ui_available(fos_api_t *api) {
    return api->clear_screen && api->goto_xy && api->set_color && api->putchar;
}

static void copy_mem(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void setup_bufs(fos_api_t *api) {
    dma_chunk = PCM8_MAX;
    if (api->sound_buf_size) {
        uint32_t n = api->sound_buf_size();
        if (n >= 1024u && n <= 65536u) {
            dma_chunk = n;
        }
    }
    file_cap = FILE_CAP;
    ring_cap = RING_CAP;
    filebuf = 0;
    ring = 0;
    chunkbuf = 0;
    if (api->mem_alloc) {
        filebuf = (uint8_t *)api->mem_alloc(file_cap);
        ring = (uint8_t *)api->mem_alloc(ring_cap);
        chunkbuf = (uint8_t *)api->mem_alloc(dma_chunk);
    }
    if (!filebuf) {
        filebuf = inbuf_fallback;
        file_cap = IN_MAX;
    }
    if (!ring) {
        ring = pcm_fallback;
        ring_cap = PCM8_MAX;
    }
    if (!chunkbuf) {
        if (ring && ring_cap > dma_chunk) {
            ring_cap -= dma_chunk;
            chunkbuf = ring + ring_cap;
        } else {
            chunkbuf = chunk_fallback;
            dma_chunk = PCM8_MAX;
        }
    }
    ring_r = ring_w = ring_n = 0;
    prefilled = 0;
    have_seek = 0;
}

static void ring_reset(void) {
    ring_r = ring_w = ring_n = 0;
    prefilled = 0;
}

static void audio_cut(fos_api_t *api) {
    if (api->sound_stop) {
        api->sound_stop();
    }
    ring_reset();
    ui.live = 0;
}

static uint32_t clamp_ms(int64_t ms, uint32_t total) {
    if (ms < 0) {
        return 0;
    }
    if (total && (uint64_t)ms > (uint64_t)total) {
        return total;
    }
    if ((uint64_t)ms >= (uint64_t)SEEK_TAIL) {
        return SEEK_TAIL;
    }
    return (uint32_t)ms;
}

/* Decode position minus PCM still sitting in the ring, so seek matches the bar. */
static uint32_t playhead_ms(void) {
    uint32_t ms = ui.pos_ms;
    uint32_t buf;
    if (ui.rate && ring_n) {
        buf = (uint32_t)(((uint64_t)ring_n * 1000ull) / ui.rate);
        if (ms > buf) {
            ms -= buf;
        } else {
            ms = 0;
        }
    }
    return ms;
}

static void ring_push(const uint8_t *src, uint32_t n) {
    while (n) {
        uint32_t space = ring_cap - ring_w;
        uint32_t m = n < space ? n : space;
        copy_mem(ring + ring_w, src, m);
        ring_w += m;
        if (ring_w == ring_cap) {
            ring_w = 0;
        }
        ring_n += m;
        src += m;
        n -= m;
    }
}

static void ring_pop(uint8_t *dst, uint32_t n) {
    while (n) {
        uint32_t avail = ring_cap - ring_r;
        uint32_t m = n < avail ? n : avail;
        copy_mem(dst, ring + ring_r, m);
        ring_r += m;
        if (ring_r == ring_cap) {
            ring_r = 0;
        }
        ring_n -= m;
        dst += m;
        n -= m;
    }
}

static void put_xy(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg, unsigned char c) {
    api->set_color(fg, bg);
    api->goto_xy(x, y);
    api->putchar((char)c);
}

static void fill_rect(fos_api_t *api, int x, int y, int w, int h,
                      uint8_t fg, uint8_t bg, unsigned char c) {
    int row, col;
    for (row = 0; row < h; row++) {
        for (col = 0; col < w; col++) {
            put_xy(api, x + col, y + row, fg, bg, c);
        }
    }
}

static void put_str(fos_api_t *api, int x, int y, uint8_t fg, uint8_t bg,
                    const char *s, int maxn) {
    int i;
    for (i = 0; s && s[i] && (maxn <= 0 || i < maxn); i++) {
        put_xy(api, x + i, y, fg, bg, (unsigned char)s[i]);
    }
}

static void put_u32(fos_api_t *api, int *x, int y, uint8_t fg, uint8_t bg, uint32_t v) {
    char tmp[10];
    int n = 0;
    int i;
    if (v == 0) {
        put_xy(api, *x, y, fg, bg, '0');
        (*x)++;
        return;
    }
    while (v && n < 10) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = n - 1; i >= 0; i--) {
        put_xy(api, *x, y, fg, bg, (unsigned char)tmp[i]);
        (*x)++;
    }
}

static void format_time(uint32_t ms, char *out) {
    uint32_t s = ms / 1000u;
    uint32_t m = s / 60u;
    s %= 60u;
    if (m > 99u) {
        m = 99u;
    }
    out[0] = (char)('0' + (m / 10u));
    out[1] = (char)('0' + (m % 10u));
    out[2] = ':';
    out[3] = (char)('0' + (s / 10u));
    out[4] = (char)('0' + (s % 10u));
    out[5] = 0;
}

static void inner_fill(fos_api_t *api, int row, uint8_t fg, uint8_t bg) {
    fill_rect(api, win_x + 1, win_y + row, WIN_W - 2, 1, fg, bg, ' ');
}

static void draw_frame(fos_api_t *api) {
    int x, y, i;
    const char *title = " PLAY ";
    int title_len = 6;
    int title_x;

    api->set_color(FG_DESK, BG_DESK);
    api->clear_screen();

    fill_rect(api, win_x + 2, win_y + 1, WIN_W, WIN_H, 7, BG_SHADOW, ' ');
    fill_rect(api, win_x, win_y, WIN_W, WIN_H, FG_WIN, BG_WIN, ' ');

    for (i = 1; i < WIN_W - 1; i++) {
        put_xy(api, win_x + i, win_y, FG_WIN, BG_WIN, CH_H);
        put_xy(api, win_x + i, win_y + WIN_H - 1, FG_WIN, BG_WIN, CH_H);
    }
    for (i = 1; i < WIN_H - 1; i++) {
        put_xy(api, win_x, win_y + i, FG_WIN, BG_WIN, CH_V);
        put_xy(api, win_x + WIN_W - 1, win_y + i, FG_WIN, BG_WIN, CH_V);
    }
    put_xy(api, win_x, win_y, FG_WIN, BG_WIN, CH_TL);
    put_xy(api, win_x + WIN_W - 1, win_y, FG_WIN, BG_WIN, CH_TR);
    put_xy(api, win_x, win_y + WIN_H - 1, FG_WIN, BG_WIN, CH_BL);
    put_xy(api, win_x + WIN_W - 1, win_y + WIN_H - 1, FG_WIN, BG_WIN, CH_BR);

    title_x = win_x + (WIN_W - title_len) / 2;
    inner_fill(api, 1, FG_TITLE, BG_TITLE);
    put_xy(api, win_x + 2, win_y + 1, FG_TITLE, BG_TITLE, CH_NOTE);
    put_str(api, title_x, win_y + 1, FG_TITLE, BG_TITLE, title, title_len);

    inner_fill(api, WIN_H - 2, FG_MUTED, BG_WIN);
    put_xy(api, win_x + 2, win_y + WIN_H - 2, FG_MUTED, BG_WIN, CH_LEFT);
    put_xy(api, win_x + 3, win_y + WIN_H - 2, FG_MUTED, BG_WIN, CH_RIGHT);
    put_str(api, win_x + 4, win_y + WIN_H - 2, FG_MUTED, BG_WIN, " 5s  ", 0);
    put_xy(api, win_x + 9, win_y + WIN_H - 2, FG_MUTED, BG_WIN, CH_UP);
    put_xy(api, win_x + 10, win_y + WIN_H - 2, FG_MUTED, BG_WIN, CH_DOWN);
    put_str(api, win_x + 11, win_y + WIN_H - 2, FG_MUTED, BG_WIN, " 30s  q quit", 0);

    /* Park the cursor on the help line. */
    x = win_x + 2;
    y = win_y + WIN_H - 2;
    api->set_color(FG_MUTED, BG_WIN);
    api->goto_xy(x, y);
}

static void draw_filename(fos_api_t *api) {
    const char *name = base_name(ui.path);
    inner_fill(api, 3, FG_WIN, BG_WIN);
    put_str(api, win_x + 2, win_y + 3, FG_WIN, BG_WIN, name, WIN_W - 4);
}

static void draw_meta(fos_api_t *api) {
    int x = win_x + 2;
    int y = win_y + 4;

    inner_fill(api, 4, FG_WIN, BG_WIN);
    if (!ui.kind) {
        return;
    }
    put_str(api, x, y, FG_BAR, BG_WIN, ui.kind, 0);
    x += (int)cstr_len(ui.kind);
    if (ui.rate) {
        put_str(api, x, y, FG_WIN, BG_WIN, "  ", 2);
        x += 2;
        put_u32(api, &x, y, FG_WIN, BG_WIN, ui.rate);
        put_str(api, x, y, FG_WIN, BG_WIN, " Hz", 0);
        x += 3;
    }
    if (ui.ch) {
        put_str(api, x, y, FG_WIN, BG_WIN, "  ", 2);
        x += 2;
        put_str(api, x, y, FG_WIN, BG_WIN, ui.ch >= 2 ? "stereo" : "mono", 0);
        x += ui.ch >= 2 ? 6 : 4;
    }
    if (ui.bits) {
        put_str(api, x, y, FG_WIN, BG_WIN, "  ", 2);
        x += 2;
        put_u32(api, &x, y, FG_WIN, BG_WIN, ui.bits);
        put_str(api, x, y, FG_WIN, BG_WIN, "-bit", 0);
    }
}

static void draw_time_bar(fos_api_t *api, int force) {
    char tpos[6];
    char ttot[8];
    uint32_t filled = 0;
    uint32_t sec = ui.pos_ms / 1000u;
    int x;
    int y;
    uint32_t i;

    if (ui.total) {
        filled = (uint32_t)(((uint64_t)ui.pos * BAR_W) / ui.total);
        if (filled > BAR_W) {
            filled = BAR_W;
        }
    }
    if (!force && ui.painted && filled == (uint32_t)ui.last_filled && sec == ui.last_sec) {
        return;
    }

    format_time(ui.pos_ms, tpos);
    if (ui.total_ms) {
        format_time(ui.total_ms, ttot);
    } else {
        ttot[0] = '-';
        ttot[1] = '-';
        ttot[2] = ':';
        ttot[3] = '-';
        ttot[4] = '-';
        ttot[5] = 0;
    }

    y = win_y + 6;
    inner_fill(api, 6, FG_WIN, BG_WIN);
    put_str(api, win_x + 2, y, FG_WIN, BG_WIN, tpos, 5);
    put_str(api, win_x + 7, y, FG_MUTED, BG_WIN, "/", 1);
    put_str(api, win_x + 9, y, FG_WIN, BG_WIN, ttot, 5);

    y = win_y + 7;
    inner_fill(api, 7, FG_WIN, BG_WIN);
    put_xy(api, win_x + 2, y, FG_WIN, BG_WIN, '[');
    for (i = 0; i < BAR_W; i++) {
        if (i < filled) {
            put_xy(api, win_x + 3 + (int)i, y, FG_TITLE, BG_TITLE, CH_FILL);
        } else {
            put_xy(api, win_x + 3 + (int)i, y, FG_EMPTY, BG_WIN, CH_EMPTY);
        }
    }
    put_xy(api, win_x + 3 + BAR_W, y, FG_WIN, BG_WIN, ']');

    y = win_y + 9;
    inner_fill(api, 9, ui.status_fg, BG_WIN);
    if (ui.status) {
        put_str(api, win_x + 2, y, ui.status_fg, BG_WIN, ui.status, WIN_W - 4);
    }

    ui.last_filled = (int)filled;
    ui.last_sec = sec;
    x = win_x + 2;
    api->set_color(FG_MUTED, BG_WIN);
    api->goto_xy(x, win_y + WIN_H - 2);
}

static void ui_paint(fos_api_t *api) {
    if (!ui_available(api)) {
        return;
    }
    draw_frame(api);
    draw_filename(api);
    draw_meta(api);
    draw_time_bar(api, 1);
    ui.painted = 1;
}

static void ui_begin(fos_api_t *api, const char *path) {
    ui.path = path ? path : "";
    ui.kind = 0;
    ui.status = "Ready";
    ui.status_fg = FG_MUTED;
    ui.rate = 0;
    ui.ch = 0;
    ui.bits = 0;
    ui.pos = 0;
    ui.total = 0;
    ui.pos_ms = 0;
    ui.total_ms = 0;
    ui.live = 0;
    ui.live_base_ms = 0;
    ui.live_chunk_ms = 0;
    ui.live_start = 0;
    ui.painted = 0;
    ui.last_filled = -1;
    ui.last_sec = 0xFFFFFFFFu;
    ui_paint(api);
}

static void ui_set_status(fos_api_t *api, const char *msg, uint8_t fg) {
    ui.status = msg;
    ui.status_fg = fg;
    if (ui.painted) {
        draw_time_bar(api, 1);
    } else if (ui_available(api)) {
        ui_paint(api);
    } else {
        api->write_line(msg);
    }
}

static void ui_progress(fos_api_t *api, uint32_t pos, uint32_t total,
                        uint32_t pos_ms, uint32_t total_ms) {
    ui.pos = pos;
    ui.total = total;
    ui.pos_ms = pos_ms;
    ui.total_ms = total_ms;
    if (ui.painted) {
        draw_time_bar(api, 0);
    }
}

static void ui_wait_key(fos_api_t *api) {
    fos_key_event_t ev;
    if (!api->has_key || !api->read_key) {
        return;
    }
    __asm__ volatile("sti");
    for (;;) {
        if (api->has_key()) {
            ev = api->read_key();
            if (ev.type != FOS_KEY_NONE) {
                return;
            }
        }
        __asm__ volatile("pause");
    }
}

static void drain_keys(fos_api_t *api) {
    int n = 0;
    if (!api->has_key || !api->read_key) {
        return;
    }
    while (n < 32 && api->has_key()) {
        (void)api->read_key();
        n++;
    }
}

static int is_quit_key(fos_key_event_t ev) {
    if (ev.type == FOS_KEY_CHAR) {
        return ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 27 || ev.ch == 3;
    }
    return 0;
}

static int apply_seek_key(fos_key_event_t ev, int64_t *t) {
    switch (ev.type) {
    case FOS_KEY_LEFT:
        *t -= (int64_t)SEEK_STEP_MS;
        return 1;
    case FOS_KEY_RIGHT:
        *t += (int64_t)SEEK_STEP_MS;
        return 1;
    case FOS_KEY_UP:
        *t -= (int64_t)SEEK_JUMP_MS;
        return 1;
    case FOS_KEY_DOWN:
        *t += (int64_t)SEEK_JUMP_MS;
        return 1;
    case FOS_KEY_PAGEUP:
        *t -= (int64_t)SEEK_PAGE_MS;
        return 1;
    case FOS_KEY_PAGEDOWN:
        *t += (int64_t)SEEK_PAGE_MS;
        return 1;
    case FOS_KEY_HOME:
        *t = 0;
        return 1;
    case FOS_KEY_END:
        *t = ui.total_ms ? (int64_t)ui.total_ms : (int64_t)SEEK_TAIL;
        return 1;
    default:
        return 0;
    }
}

static int poll_cmd(fos_api_t *api) {
    int n = 0;
    int got_seek = 0;
    int64_t t = 0;

    if (!api->has_key || !api->read_key) {
        return PLAY_OK;
    }
    while (n < 16 && api->has_key()) {
        fos_key_event_t ev = api->read_key();
        n++;
        if (is_quit_key(ev)) {
            return PLAY_QUIT;
        }
        if (ev.type == FOS_KEY_NONE) {
            continue;
        }
        if (!got_seek) {
            t = (int64_t)playhead_ms();
        }
        if (apply_seek_key(ev, &t)) {
            got_seek = 1;
        }
    }
    if (got_seek) {
        seek_to_ms = clamp_ms(t, ui.total_ms);
        have_seek = 1;
        return PLAY_SEEK;
    }
    /* Drain PS/2 even when no keys are waiting so the pointer keeps moving. */
    if (api->has_key) {
        (void)api->has_key();
    }
    if (got_seek) {
        seek_to_ms = clamp_ms(t, ui.total_ms);
        have_seek = 1;
        return PLAY_SEEK;
    }
    if (api->mouse_poll) {
        fos_mouse_t m;
        int cols = 80;
        int rows = 25;
        if (api->get_term_size) {
            api->get_term_size(&cols, &rows);
        }
        if (api->mouse_poll(&m) && (m.pending & 1) && m.y >= rows - 1) {
            return PLAY_QUIT;
        }
    }
    return PLAY_OK;
}

static void ui_live_tick(fos_api_t *api) {
    uint32_t elapsed;
    uint32_t pos_ms;

    if (!ui.live || !api->get_ticks_ms) {
        return;
    }
    elapsed = (uint32_t)(api->get_ticks_ms() - ui.live_start);
    if (elapsed > ui.live_chunk_ms) {
        elapsed = ui.live_chunk_ms;
    }
    pos_ms = ui.live_base_ms + elapsed;
    ui_progress(api, ui.pos, ui.total, pos_ms, ui.total_ms);
}

/* Wait for a DMA slot, or for the stream to finish. Never kill a live
 * stream on a short timer — a CD-quality song is minutes long. */
static int wait_audio(fos_api_t *api, int for_slot) {
    uint64_t start;

    __asm__ volatile("sti");
    start = api->get_ticks_ms ? api->get_ticks_ms() : 0;
    for (;;) {
        if (for_slot) {
            if (!api->sound_can_queue || api->sound_can_queue()) {
                return 0;
            }
        } else if (!api->sound_playing || !api->sound_playing()) {
            break;
        }
        int cmd = poll_cmd(api);
        if (cmd == PLAY_QUIT) {
            api->sound_stop();
            ui.live = 0;
            ui_set_status(api, "Stopped", FG_MUTED);
            return PLAY_QUIT;
        }
        if (cmd == PLAY_SEEK) {
            api->sound_stop();
            ui.live = 0;
            return PLAY_SEEK;
        }
        ui_live_tick(api);
        if (api->get_ticks_ms) {
            uint64_t now = api->get_ticks_ms();
            if (for_slot) {
                /* Do not call sound_playing() here: that starts a primed
                 * single half before the second DMA buffer is full. */
                if (now - start > 30000ull) {
                    if (api->sound_stop) {
                        api->sound_stop();
                    }
                    break;
                }
            } else {
                int live = api->sound_playing && api->sound_playing();
                if (!live && now - start > 8000ull) {
                    if (api->sound_stop) {
                        api->sound_stop();
                    }
                    break;
                }
            }
        }
        __asm__ volatile("pause");
    }
    ui.live = 0;
    return 0;
}

static int wait_idle(fos_api_t *api) {
    return wait_audio(api, 0);
}

static int wait_slot(fos_api_t *api) {
    if (api->sound_can_queue) {
        return wait_audio(api, 1);
    }
    return wait_idle(api);
}

static int queue_direct(fos_api_t *api, const uint8_t *pcm, uint32_t n, uint32_t rate) {
    uint32_t chunk_ms;

    if (n == 0) {
        return 0;
    }
    if (api->sound_start) {
        if (api->sound_start(pcm, n, rate) != 0) {
            return -1;
        }
    } else if (api->sound_play(pcm, n, rate) != 0) {
        return -1;
    }
    chunk_ms = rate ? (uint32_t)(((uint64_t)n * 1000ull) / rate) : 0;
    ui.live = 1;
    ui.live_base_ms = ui.pos_ms;
    ui.live_chunk_ms = chunk_ms;
    ui.live_start = api->get_ticks_ms ? api->get_ticks_ms() : 0;
    return 0;
}

/* block: 0 = don't wait, 1 = wait for one DMA slot, 2 = flush a short tail. */
static int feed_card(fos_api_t *api, uint32_t rate, int block) {
    int fed = 0;

    if (!prefilled && block != 2) {
        if (ring_n < dma_chunk * 2u && ring_n < ring_cap) {
            return 0;
        }
        prefilled = 1;
    }
    for (;;) {
        uint32_t n;
        int q;
        if (ring_n == 0) {
            return 0;
        }
        if (ring_n < dma_chunk) {
            if (block != 2) {
                return 0;
            }
            n = ring_n;
        } else {
            n = dma_chunk;
        }
        if (api->sound_can_queue && !api->sound_can_queue()) {
            int w;
            if (block == 0 || (block == 1 && fed)) {
                return 0;
            }
            w = wait_slot(api);
            if (w != PLAY_OK) {
                return w;
            }
        }
        ring_pop(chunkbuf, n);
        q = queue_direct(api, chunkbuf, n, rate);
        if (q != 0) {
            return q;
        }
        fed = 1;
    }
}

static int queue_pcm(fos_api_t *api, const uint8_t *pcm, uint32_t n, uint32_t rate) {
    uint32_t off = 0;

    if (n == 0) {
        return 0;
    }
    while (off < n) {
        uint32_t space = ring_cap - ring_n;
        uint32_t m;
        if (space == 0) {
            int f = feed_card(api, rate, 1);
            if (f != PLAY_OK) {
                return f;
            }
            continue;
        }
        m = n - off;
        if (m > space) {
            m = space;
        }
        ring_push(pcm + off, m);
        off += m;
        {
            int f = feed_card(api, rate, 0);
            if (f != PLAY_OK) {
                return f;
            }
        }
    }
    return 0;
}

static int finish_playback(fos_api_t *api) {
    int f = feed_card(api, ui.rate ? ui.rate : 44100, 2);
    if (f != PLAY_OK) {
        return f;
    }
    return wait_idle(api);
}

static uint32_t bytes_to_ms(uint32_t bytes, uint32_t frame, uint32_t rate) {
    uint64_t samples;
    if (frame == 0 || rate == 0) {
        return 0;
    }
    samples = (uint64_t)bytes / frame;
    return (uint32_t)((samples * 1000ull) / rate);
}

static uint32_t s16_to_u8_mono(const int16_t *pcm, int samples, int ch, uint8_t *out, uint32_t cap) {
    uint32_t o = 0;
    int i;

    if (ch < 1) {
        ch = 1;
    }
    for (i = 0; i < samples && o < cap; i++) {
        int32_t s;
        if (ch == 1) {
            s = pcm[i];
        } else {
            s = ((int32_t)pcm[i * ch] + (int32_t)pcm[i * ch + 1]) / 2;
        }
        out[o++] = (uint8_t)((s / 256) + 128);
    }
    return o;
}

static uint32_t u8_to_mono(const uint8_t *pcm, uint32_t bytes, int ch, uint8_t *out, uint32_t cap) {
    uint32_t i, o = 0;

    if (ch < 1) {
        ch = 1;
    }
    for (i = 0; i + (uint32_t)ch <= bytes && o < cap; i += (uint32_t)ch) {
        out[o++] = pcm[i];
    }
    return o;
}

static uint32_t s16le_to_u8_mono(const uint8_t *pcm, uint32_t bytes, int ch, uint8_t *out, uint32_t cap) {
    uint32_t step = (uint32_t)ch * 2u;
    uint32_t i, o = 0;

    if (ch < 1) {
        ch = 1;
        step = 2;
    }
    for (i = 0; i + step <= bytes && o < cap; i += step) {
        int16_t left = (int16_t)(pcm[i] | ((uint16_t)pcm[i + 1] << 8));
        int32_t s = left;
        if (ch >= 2) {
            int16_t right = (int16_t)(pcm[i + 2] | ((uint16_t)pcm[i + 3] << 8));
            s = (s + right) / 2;
        }
        out[o++] = (uint8_t)((s / 256) + 128);
    }
    return o;
}

static int parse_wav(fos_api_t *api, const char *path, uint32_t file_size,
                     uint32_t *data_off, uint32_t *data_len, uint32_t *rate,
                     uint16_t *ch, uint16_t *bits) {
    uint8_t riff[12];
    uint8_t chdr[8];
    uint32_t got = 0;
    uint32_t pos;
    int got_fmt = 0;
    int got_data = 0;

    if (file_size < 44) {
        return -1;
    }
    if (api->read_at(path, 0, riff, 12, &got) != 0 || got < 12) {
        return -1;
    }
    if (!(riff[0] == 'R' && riff[1] == 'I' && riff[2] == 'F' && riff[3] == 'F' &&
          riff[8] == 'W' && riff[9] == 'A' && riff[10] == 'V' && riff[11] == 'E')) {
        return -1;
    }

    pos = 12;
    while (pos + 8 <= file_size) {
        uint32_t cksz;
        uint32_t payload;
        uint32_t next;
        if (api->read_at(path, pos, chdr, 8, &got) != 0 || got < 8) {
            break;
        }
        cksz = rd32(chdr + 4);
        payload = pos + 8;
        next = payload + ((cksz + 1u) & ~1u);
        if (chdr[0] == 'f' && chdr[1] == 'm' && chdr[2] == 't') {
            uint8_t fmt[16];
            uint32_t n = 0;
            if (api->read_at(path, payload, fmt, 16, &n) != 0 || n < 16) {
                return -1;
            }
            if (rd16(fmt) != 1) {
                return -2;
            }
            *ch = rd16(fmt + 2);
            *rate = rd32(fmt + 4);
            *bits = rd16(fmt + 14);
            got_fmt = 1;
        } else if (chdr[0] == 'd' && chdr[1] == 'a' && chdr[2] == 't' && chdr[3] == 'a') {
            *data_off = payload;
            *data_len = cksz;
            if (*data_off > file_size) {
                return -1;
            }
            if (*data_off + *data_len > file_size) {
                *data_len = file_size - *data_off;
            }
            got_data = 1;
            break;
        }
        if (next <= pos) {
            break;
        }
        pos = next;
    }

    if (!got_fmt || !got_data || *ch < 1 || (*bits != 8 && *bits != 16)) {
        return -1;
    }
    return 0;
}

static void wav_apply_seek(fos_api_t *api, uint32_t *pos, uint32_t data_len,
                           uint32_t frame, uint32_t rate) {
    uint64_t samples;
    uint32_t ms;

    audio_cut(api);
    have_seek = 0;
    ms = seek_to_ms;
    if (ms == SEEK_TAIL || frame == 0 || rate == 0) {
        *pos = data_len;
    } else {
        samples = ((uint64_t)ms * rate) / 1000ull;
        if (samples > (uint64_t)(data_len / frame)) {
            *pos = data_len;
        } else {
            *pos = (uint32_t)(samples * frame);
            *pos -= *pos % frame;
        }
    }
    ms = bytes_to_ms(*pos, frame, rate);
    ui.status = "Playing";
    ui.status_fg = FG_OK;
    ui_progress(api, *pos, data_len, ms, bytes_to_ms(data_len, frame, rate));
}

static int wait_done_keys(fos_api_t *api) {
    __asm__ volatile("sti");
    for (;;) {
        int cmd = poll_cmd(api);
        if (cmd != PLAY_OK) {
            return cmd;
        }
        __asm__ volatile("pause");
    }
}

static int play_wav(fos_api_t *api, const char *path, uint32_t file_size) {
    uint32_t data_off = 0, data_len = 0, rate = 8000, pos = 0;
    uint16_t ch = 1, bits = 8;
    uint32_t frame;
    int parsed;

    parsed = parse_wav(api, path, file_size, &data_off, &data_len, &rate, &ch, &bits);
    if (parsed == -2) {
        ui_set_status(api, "WAV is not PCM", FG_ERR);
        ui_wait_key(api);
        return -1;
    }
    if (parsed != 0) {
        ui_set_status(api, "Not a PCM WAV", FG_ERR);
        ui_wait_key(api);
        return -1;
    }
    frame = (bits / 8u) * ch;
    if (frame == 0) {
        ui_set_status(api, "Bad WAV format", FG_ERR);
        ui_wait_key(api);
        return -1;
    }

    ui.kind = "WAV";
    ui.rate = rate;
    ui.ch = ch;
    ui.bits = bits;
    ui.status = "Playing";
    ui.status_fg = FG_OK;
    draw_meta(api);
    ui_progress(api, 0, data_len, 0, bytes_to_ms(data_len, frame, rate));
    drain_keys(api);
    __asm__ volatile("sti");

    for (;;) {
        int cmd;

        ui.status = "Playing";
        ui.status_fg = FG_OK;
        while (pos < data_len) {
            uint32_t want = data_len - pos;
            uint32_t got = 0;
            uint32_t src_off;
            int q;
            int jumped = 0;

            cmd = poll_cmd(api);
            if (cmd == PLAY_QUIT) {
                api->sound_stop();
                ui_set_status(api, "Stopped", FG_MUTED);
                return 0;
            }
            if (cmd == PLAY_SEEK) {
                wav_apply_seek(api, &pos, data_len, frame, rate);
                continue;
            }
            if (want > file_cap) {
                want = file_cap;
            }
            want -= want % frame;
            if (want == 0) {
                break;
            }
            if (api->read_at(path, data_off + pos, filebuf, want, &got) != 0 || got == 0) {
                break;
            }
            got -= got % frame;
            src_off = 0;
            while (src_off < got) {
                uint8_t tmp[2048];
                uint32_t n8;
                uint32_t take = got - src_off;
                if (bits == 8) {
                    n8 = u8_to_mono(filebuf + src_off, take, ch, tmp, 2048);
                    src_off += n8 * (uint32_t)ch;
                } else {
                    n8 = s16le_to_u8_mono(filebuf + src_off, take, ch, tmp, 2048);
                    src_off += n8 * (uint32_t)ch * 2u;
                }
                if (n8 == 0) {
                    break;
                }
                q = queue_pcm(api, tmp, n8, rate);
                if (q == PLAY_QUIT) {
                    return 0;
                }
                if (q == PLAY_SEEK) {
                    wav_apply_seek(api, &pos, data_len, frame, rate);
                    jumped = 1;
                    break;
                }
                if (q != PLAY_OK) {
                    ui_set_status(api, "Playback failed", FG_ERR);
                    ui_wait_key(api);
                    return -1;
                }
            }
            if (jumped) {
                continue;
            }
            pos += got;
            ui_progress(api, pos, data_len,
                        bytes_to_ms(pos, frame, rate),
                        bytes_to_ms(data_len, frame, rate));
        }

        cmd = finish_playback(api);
        if (cmd == PLAY_QUIT) {
            return 0;
        }
        if (cmd == PLAY_SEEK) {
            wav_apply_seek(api, &pos, data_len, frame, rate);
            continue;
        }
        ui_progress(api, data_len, data_len,
                    bytes_to_ms(data_len, frame, rate),
                    bytes_to_ms(data_len, frame, rate));
        ui_set_status(api, "Done", FG_OK);
        cmd = wait_done_keys(api);
        if (cmd == PLAY_SEEK) {
            wav_apply_seek(api, &pos, data_len, frame, rate);
            continue;
        }
        return 0;
    }
}

static uint32_t skip_id3(fos_api_t *api, const char *path, uint32_t file_size) {
    uint8_t h[10];
    uint32_t got = 0;

    if (file_size < 10) {
        return 0;
    }
    if (api->read_at(path, 0, h, 10, &got) != 0 || got < 10) {
        return 0;
    }
    if (h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        uint32_t n = ((uint32_t)(h[6] & 0x7F) << 21) | ((uint32_t)(h[7] & 0x7F) << 14) |
                     ((uint32_t)(h[8] & 0x7F) << 7) | (uint32_t)(h[9] & 0x7F);
        n += 10;
        if (h[5] & 0x10) {
            n += 10;
        }
        if (n > file_size) {
            n = file_size;
        }
        return n;
    }
    return 0;
}

static int looks_mp3(const uint8_t *p, uint32_t n) {
    uint32_t i;
    if (n >= 3 && p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
        return 1;
    }
    for (i = 0; i + 2 < n && i < 64; i++) {
        if (p[i] == 0xFF && (p[i + 1] & 0xE0) == 0xE0) {
            return 1;
        }
    }
    return 0;
}

static void mp3_apply_seek(fos_api_t *api, uint32_t *file_off, uint32_t *in_off,
                           uint32_t *fill, int *eof, uint64_t *played,
                           uint32_t rate, uint32_t id3, uint32_t payload,
                           int *skip_frame) {
    uint32_t ms = seek_to_ms;
    uint32_t body;

    audio_cut(api);
    have_seek = 0;
    fos_mp3_init();
    *in_off = 0;
    *fill = 0;
    *eof = 0;
    *skip_frame = 0;

    if (ms == SEEK_TAIL || (ui.total_ms && ms >= ui.total_ms)) {
        *file_off = id3 + payload;
        *eof = 1;
        *played = (rate && ui.total_ms) ? ((uint64_t)ui.total_ms * rate) / 1000ull : *played;
        ui.status = "Playing";
        ui.status_fg = FG_OK;
        ui_progress(api, payload, payload, ui.total_ms, ui.total_ms);
        return;
    }

    if (ui.total_ms) {
        body = (uint32_t)(((uint64_t)ms * payload) / ui.total_ms);
    } else {
        body = ms * 16u;
        if (body > payload) {
            body = payload;
        }
    }
    *file_off = id3 + body;
    if (*file_off < id3) {
        *file_off = id3;
    }
    if (*file_off > id3 + payload) {
        *file_off = id3 + payload;
    }
    if (rate) {
        *played = ((uint64_t)ms * rate) / 1000ull;
    }
    *skip_frame = (*file_off > id3);
    ui.status = "Playing";
    ui.status_fg = FG_OK;
    ui_progress(api, body, payload, ms, ui.total_ms);
}

static int looks_midi(const uint8_t *p, uint32_t n) {
    return n >= 4 && p[0] == 'M' && p[1] == 'T' && p[2] == 'h' && p[3] == 'd';
}

static int load_all(fos_api_t *api, const char *path, uint8_t **out, uint32_t *out_len) {
    uint32_t size = 0;
    int is_dir = 0;
    uint8_t *buf;
    uint32_t off = 0;

    if (!api->stat_file || api->stat_file(path, &size, &is_dir) != 0 || is_dir || size == 0) {
        return -1;
    }
    if (size > 32u * 1024u * 1024u) {
        return -1;
    }
    if (!api->mem_alloc) {
        return -1;
    }
    buf = (uint8_t *)api->mem_alloc(size);
    if (!buf) {
        return -1;
    }
    while (off < size) {
        uint32_t got = 0;
        uint32_t n = size - off;
        if (n > 32768u) {
            n = 32768u;
        }
        __asm__ volatile("sti");
        if (api->read_at(path, off, buf + off, n, &got) != 0 || got == 0) {
            api->mem_free(buf);
            return -1;
        }
        off += got;
    }
    *out = buf;
    *out_len = size;
    return 0;
}

static const char *find_sf2(fos_api_t *api) {
    static const char *paths[] = {
        "\\FOS\\GM.SF2",
        "GM.SF2",
        "\\GM.SF2",
        0
    };
    int i;
    for (i = 0; paths[i]; i++) {
        uint32_t size = 0;
        int is_dir = 0;
        if (api->stat_file && api->stat_file(paths[i], &size, &is_dir) == 0 &&
            !is_dir && size > 64u) {
            return paths[i];
        }
    }
    return 0;
}

static int play_midi(fos_api_t *api, const char *path, uint32_t file_size) {
    uint8_t *mid = 0;
    uint8_t *sf2 = 0;
    uint32_t mid_len = 0;
    uint32_t sf2_len = 0;
    const char *sfpath;
    uint32_t rate = 22050;
    uint8_t pcm[2048];
    int n;

    (void)file_size;
    ui.kind = "MIDI";
    ui.bits = 16;
    ui.ch = 1;
    ui.rate = rate;
    draw_meta(api);

    if (load_all(api, path, &mid, &mid_len) != 0) {
        ui_set_status(api, "Cannot read MIDI", FG_ERR);
        ui_wait_key(api);
        return -1;
    }
    sfpath = find_sf2(api);
    if (!sfpath) {
        api->mem_free(mid);
        if (api->show_error) {
            api->show_error("Need \\FOS\\GM.SF2 (rebuild the disk image)");
        } else {
            ui_set_status(api, "Need \\FOS\\GM.SF2", FG_ERR);
            ui_wait_key(api);
        }
        return -1;
    }
    ui_set_status(api, "Loading soundfont...", FG_MUTED);
    if (load_all(api, sfpath, &sf2, &sf2_len) != 0) {
        api->mem_free(mid);
        ui_set_status(api, "Cannot read GM.SF2", FG_ERR);
        ui_wait_key(api);
        return -1;
    }
    midi_fos_init(api);
    if (midi_fos_start(sf2, (int)sf2_len, mid, (int)mid_len, (int)rate) != 0) {
        api->mem_free(mid);
        api->mem_free(sf2);
        midi_fos_stop();
        ui_set_status(api, "MIDI / soundfont load failed", FG_ERR);
        ui_wait_key(api);
        return -1;
    }
    api->mem_free(mid);
    api->mem_free(sf2);
    ui.total_ms = midi_fos_length_ms();
    ui.total = ui.total_ms ? ui.total_ms : 1;
    ui.status = "Playing";
    ui.status_fg = FG_OK;
    ui_progress(api, 0, ui.total, 0, ui.total_ms);
    draw_meta(api);

    for (;;) {
        int q;
        if (have_seek) {
            unsigned ms = seek_to_ms;
            audio_cut(api);
            have_seek = 0;
            if (ms == SEEK_TAIL) {
                ms = ui.total_ms;
            }
            midi_fos_seek_ms(ms);
            ui_progress(api, midi_fos_time_ms(), ui.total, midi_fos_time_ms(), ui.total_ms);
            continue;
        }
        n = midi_fos_render_u8(pcm, 2048);
        if (n <= 0) {
            break;
        }
        q = queue_pcm(api, pcm, (uint32_t)n, rate);
        ui_progress(api, midi_fos_time_ms(), ui.total, midi_fos_time_ms(), ui.total_ms);
        if (q == PLAY_QUIT) {
            midi_fos_stop();
            return 0;
        }
        if (q == PLAY_SEEK) {
            continue;
        }
    }
    {
        int cmd = finish_playback(api);
        midi_fos_stop();
        if (cmd == PLAY_QUIT) {
            return 0;
        }
        if (cmd == PLAY_SEEK) {
            return play_midi(api, path, file_size);
        }
    }
    ui_progress(api, ui.total, ui.total, ui.total_ms, ui.total_ms);
    ui_set_status(api, "Done", FG_OK);
    {
        int cmd = wait_done_keys(api);
        if (cmd == PLAY_SEEK) {
            return play_midi(api, path, file_size);
        }
    }
    return 0;
}

static int play_mp3(fos_api_t *api, const char *path, uint32_t file_size) {
    uint32_t file_off;
    uint32_t id3;
    uint32_t payload;
    uint32_t in_off = 0;
    uint32_t fill = 0;
    int eof = 0;
    int started = 0;
    int skip_frame = 0;
    uint32_t rate = 44100;
    uint64_t played = 0;

    id3 = skip_id3(api, path, file_size);
    file_off = id3;
    payload = file_size > id3 ? file_size - id3 : 1;
    fos_mp3_init();

    ui.kind = "MP3";
    ui.rate = 0;
    ui.ch = 0;
    ui.bits = 0;
    ui.status = "Playing";
    ui.status_fg = FG_OK;
    draw_meta(api);
    ui_progress(api, 0, payload, 0, 0);
    drain_keys(api);
    __asm__ volatile("sti");

    for (;;) {
        int cmd;

        ui.status = "Playing";
        ui.status_fg = FG_OK;
        while (!eof || fill - in_off > 16) {
            int samples;
            uint32_t n8;
            int frame_bytes = 0;
            int channels = 1;
            int hz = 44100;
            uint32_t consumed;
            uint32_t pos_ms;
            uint32_t tot_ms;
            int q;
            uint32_t avail;

            cmd = poll_cmd(api);
            if (cmd == PLAY_QUIT) {
                api->sound_stop();
                ui_set_status(api, "Stopped", FG_MUTED);
                return 0;
            }
            if (cmd == PLAY_SEEK) {
                mp3_apply_seek(api, &file_off, &in_off, &fill, &eof, &played,
                               rate, id3, payload, &skip_frame);
                continue;
            }

            avail = fill - in_off;
            if (!eof && avail < file_cap / 4) {
                uint32_t got = 0;
                uint32_t want;
                if (in_off > 0 && avail > 0) {
                    copy_mem(filebuf, filebuf + in_off, avail);
                }
                fill = avail;
                in_off = 0;
                want = file_cap - fill;
                if (want > 0) {
                    if (api->read_at(path, file_off, filebuf + fill, want, &got) != 0) {
                        ui_set_status(api, "Read failed", FG_ERR);
                        ui_wait_key(api);
                        return -1;
                    }
                    if (got == 0) {
                        eof = 1;
                    } else {
                        fill += got;
                        file_off += got;
                    }
                }
                avail = fill - in_off;
            }

            if (avail < 4) {
                break;
            }

            samples = fos_mp3_decode(filebuf + in_off, (int)avail, mp3pcm,
                                     &frame_bytes, &channels, &hz);
            if (frame_bytes <= 0) {
                if (eof) {
                    break;
                }
                in_off++;
                continue;
            }

            if ((uint32_t)frame_bytes > avail) {
                break;
            }
            in_off += (uint32_t)frame_bytes;

            if (samples <= 0) {
                continue;
            }
            if (!started) {
                rate = (uint32_t)hz;
                if (rate < 4000) {
                    rate = 4000;
                }
                if (rate > 44100) {
                    rate = 44100;
                }
                ui.rate = rate;
                ui.ch = (uint16_t)channels;
                draw_meta(api);
                started = 1;
            }
            if (skip_frame) {
                skip_frame = 0;
                continue;
            }
            {
                uint8_t tmp[2304];
                n8 = s16_to_u8_mono(mp3pcm, samples, channels, tmp, 2304);
                q = queue_pcm(api, tmp, n8, rate);
                if (q == PLAY_QUIT) {
                    return 0;
                }
                if (q == PLAY_SEEK) {
                    mp3_apply_seek(api, &file_off, &in_off, &fill, &eof, &played,
                                   rate, id3, payload, &skip_frame);
                    continue;
                }
                if (q != PLAY_OK) {
                    ui_set_status(api, "Playback failed", FG_ERR);
                    ui_wait_key(api);
                    return -1;
                }
            }
            played += (uint32_t)samples;
            consumed = file_off > id3 ? file_off - id3 : 0;
            if (consumed > payload) {
                consumed = payload;
            }
            pos_ms = rate ? (uint32_t)((played * 1000ull) / rate) : 0;
            tot_ms = 0;
            if (consumed > 0 && pos_ms > 0) {
                tot_ms = (uint32_t)(((uint64_t)pos_ms * payload) / consumed);
            }
            ui_progress(api, consumed, payload, pos_ms, tot_ms);
        }

        cmd = finish_playback(api);
        if (cmd == PLAY_QUIT) {
            return 0;
        }
        if (cmd == PLAY_SEEK) {
            mp3_apply_seek(api, &file_off, &in_off, &fill, &eof, &played,
                           rate, id3, payload, &skip_frame);
            continue;
        }
        {
            uint32_t pos_ms = rate ? (uint32_t)((played * 1000ull) / rate) : 0;
            ui_progress(api, payload, payload, pos_ms, pos_ms ? pos_ms : ui.total_ms);
        }
        ui_set_status(api, "Done", FG_OK);
        cmd = wait_done_keys(api);
        if (cmd == PLAY_SEEK) {
            mp3_apply_seek(api, &file_off, &in_off, &fill, &eof, &played,
                           rate, id3, payload, &skip_frame);
            continue;
        }
        return 0;
    }
}

void com_main(void) {
    fos_api_t *api = (fos_api_t *)FOS_API_ADDR;
    const char *path;
    uint32_t size = 0;
    int is_dir = 0;
    uint8_t sniff[16];
    uint32_t got = 0;

    if (!api || api->magic != FOS_API_MAGIC) {
        return;
    }

    init_geometry(api);
    path = skip_ws(api->cmdline);
    if (api->begin_direct) {
        api->begin_direct();
    }
    ui_begin(api, path);
    setup_bufs(api);

    if (!api->sound_present || !api->sound_play || !api->read_at || !api->stat_file) {
        if (api->show_error) {
            api->show_error("API missing - rebuild the kernel");
        } else {
            ui_set_status(api, "API missing - rebuild the kernel", FG_ERR);
            ui_wait_key(api);
        }
        goto done;
    }
    if (!api->sound_present()) {
        if (api->show_error) {
            api->show_error("No Sound Blaster (QEMU: -device sb16)");
        } else {
            ui_set_status(api, "No Sound Blaster (QEMU: -device sb16)", FG_ERR);
            ui_wait_key(api);
        }
        goto done;
    }

    if (!path[0]) {
        ui_set_status(api, "usage: play FILE.WAV / FILE.MP3 / FILE.MID", FG_MUTED);
        ui_wait_key(api);
        goto done;
    }

    if (api->stat_file(path, &size, &is_dir) != 0 || is_dir || size == 0) {
        if (api->show_error) {
            api->show_error("Cannot open file");
        } else {
            ui_set_status(api, "Cannot open file", FG_ERR);
            ui_wait_key(api);
        }
        goto done;
    }

    if (api->read_at(path, 0, sniff, sizeof(sniff), &got) != 0 || got < 4) {
        if (api->show_error) {
            api->show_error("Cannot read file");
        } else {
            ui_set_status(api, "Cannot read file", FG_ERR);
            ui_wait_key(api);
        }
        goto done;
    }

    if (got >= 12 && sniff[0] == 'R' && sniff[1] == 'I' && sniff[2] == 'F' && sniff[3] == 'F') {
        play_wav(api, path, size);
        goto done;
    }
    if (looks_midi(sniff, got)) {
        play_midi(api, path, size);
        goto done;
    }
    if (looks_mp3(sniff, got)) {
        play_mp3(api, path, size);
        goto done;
    }

    if (api->show_error) {
        api->show_error("Not WAV, MP3 or MIDI");
    } else {
        ui_set_status(api, "Not WAV, MP3 or MIDI", FG_ERR);
        ui_wait_key(api);
    }

done:
    if (api->end_direct) {
        api->end_direct();
    }
}
