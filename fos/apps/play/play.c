/*
 * play.c — Stream WAV / MP3 through the Sound Blaster.
 *
 *   play FILE.WAV
 *   play FILE.MP3
 *
 * Full-screen TUI: purple desktop, grey player window, progress bar.
 * q / Esc / Ctrl+C quit immediately while audio plays.
 */

#include "fos_api.h"
#include "mp3dec_fos.h"

#define IN_MAX     4096
#define PCM8_MAX   4096

#define BG_DESK    5  /* purple */
#define FG_DESK    13
#define BG_SHADOW  0
#define BG_WIN     7  /* light grey */
#define FG_WIN     0  /* black */
#define FG_MUTED   8
#define FG_TITLE   15
#define BG_TITLE   5
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

static uint8_t inbuf[IN_MAX];
static uint8_t pcm8[PCM8_MAX];
static int16_t mp3pcm[FOS_MP3_MAX_SAMPLES];

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
    put_str(api, win_x + 2, win_y + WIN_H - 2, FG_MUTED, BG_WIN, "q  quit", 0);

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

static int poll_quit(fos_api_t *api) {
    int n = 0;
    if (!api->has_key || !api->read_key) {
        return 0;
    }
    while (n < 8 && api->has_key()) {
        fos_key_event_t ev = api->read_key();
        n++;
        if (is_quit_key(ev)) {
            return 1;
        }
        if (ev.type == FOS_KEY_NONE) {
            break;
        }
    }
    return 0;
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

/* The driver holds at most two buffers, so nothing should take longer than a
 * couple of chunks. Anything beyond that means the card stopped reporting and
 * we drop the tail rather than lock up the machine. */
static uint32_t wait_budget_ms(void) {
    uint32_t budget = ui.live_chunk_ms * 2u + 1000u;
    return budget < 1000u ? 1000u : budget;
}

static int wait_audio(fos_api_t *api, int for_slot) {
    uint64_t start;
    uint32_t budget = wait_budget_ms();

    __asm__ volatile("sti");
    start = api->get_ticks_ms ? api->get_ticks_ms() : 0;
    for (;;) {
        if (for_slot) {
            if (api->sound_can_queue()) {
                return 0;
            }
        } else if (!api->sound_playing || !api->sound_playing()) {
            break;
        }
        if (poll_quit(api)) {
            api->sound_stop();
            ui.live = 0;
            ui_set_status(api, "Stopped", FG_MUTED);
            return 1;
        }
        ui_live_tick(api);
        if (api->get_ticks_ms && api->get_ticks_ms() - start > budget) {
            api->sound_stop();
            break;
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

static int queue_pcm(fos_api_t *api, const uint8_t *pcm, uint32_t n, uint32_t rate) {
    uint32_t chunk_ms;

    if (n == 0) {
        return 0;
    }
    if (wait_slot(api)) {
        return 1;
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

static int finish_playback(fos_api_t *api) {
    if (wait_idle(api)) {
        return 1;
    }
    return 0;
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

    while (pos < data_len) {
        uint32_t want = data_len - pos;
        uint32_t got = 0;
        uint32_t n8;
        int q;

        if (poll_quit(api)) {
            api->sound_stop();
            ui_set_status(api, "Stopped", FG_MUTED);
            return 0;
        }
        if (want > IN_MAX) {
            want = IN_MAX;
        }
        want -= want % frame;
        if (want == 0) {
            break;
        }
        if (api->read_at(path, data_off + pos, inbuf, want, &got) != 0 || got == 0) {
            break;
        }
        got -= got % frame;
        if (bits == 8) {
            n8 = u8_to_mono(inbuf, got, ch, pcm8, PCM8_MAX);
        } else {
            n8 = s16le_to_u8_mono(inbuf, got, ch, pcm8, PCM8_MAX);
        }
        q = queue_pcm(api, pcm8, n8, rate);
        if (q == 1) {
            return 0;
        }
        if (q != 0) {
            ui_set_status(api, "Playback failed", FG_ERR);
            ui_wait_key(api);
            return -1;
        }
        pos += got;
        ui_progress(api, pos, data_len,
                    bytes_to_ms(pos, frame, rate),
                    bytes_to_ms(data_len, frame, rate));
    }

    if (finish_playback(api)) {
        return 0;
    }
    ui_progress(api, data_len, data_len,
                bytes_to_ms(data_len, frame, rate),
                bytes_to_ms(data_len, frame, rate));
    ui_set_status(api, "Done", FG_OK);
    return 0;
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

static int play_mp3(fos_api_t *api, const char *path, uint32_t file_size) {
    uint32_t file_off;
    uint32_t id3;
    uint32_t payload;
    uint32_t fill = 0;
    int eof = 0;
    int started = 0;
    uint32_t rate = 44100;
    uint64_t played = 0;
    uint32_t acc = 0;

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

    while (!eof || fill > 16) {
        int samples;
        uint32_t n8;
        int frame_bytes = 0;
        int channels = 1;
        int hz = 44100;
        uint32_t consumed;
        uint32_t pos_ms;
        uint32_t tot_ms;
        int q;

        if (poll_quit(api)) {
            api->sound_stop();
            ui_set_status(api, "Stopped", FG_MUTED);
            return 0;
        }

        if (!eof && fill < IN_MAX / 2) {
            uint32_t got = 0;
            uint32_t want = IN_MAX - fill;
            if (api->read_at(path, file_off, inbuf + fill, want, &got) != 0) {
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

        if (fill < 4) {
            break;
        }

        samples = fos_mp3_decode(inbuf, (int)fill, mp3pcm, &frame_bytes, &channels, &hz);
        if (frame_bytes <= 0) {
            if (eof) {
                break;
            }
            if (fill == IN_MAX) {
                uint32_t i;
                for (i = 1; i < fill; i++) {
                    inbuf[i - 1] = inbuf[i];
                }
                fill--;
            }
            continue;
        }

        if ((uint32_t)frame_bytes > fill) {
            break;
        }
        {
            uint32_t i;
            uint32_t rest = fill - (uint32_t)frame_bytes;
            for (i = 0; i < rest; i++) {
                inbuf[i] = inbuf[i + (uint32_t)frame_bytes];
            }
            fill = rest;
        }

        if (samples <= 0) {
            continue;
        }
        if (!started) {
            rate = (uint32_t)hz;
            if (rate < 4000) {
                rate = 4000;
            }
            ui.rate = rate;
            ui.ch = (uint16_t)channels;
            draw_meta(api);
            started = 1;
        }
        {
            uint8_t tmp[2304];
            uint32_t i;
            n8 = s16_to_u8_mono(mp3pcm, samples, channels, tmp, 2304);
            for (i = 0; i < n8; i++) {
                pcm8[acc++] = tmp[i];
                if (acc == PCM8_MAX) {
                    q = queue_pcm(api, pcm8, PCM8_MAX, rate);
                    if (q == 1) {
                        return 0;
                    }
                    if (q != 0) {
                        ui_set_status(api, "Playback failed", FG_ERR);
                        ui_wait_key(api);
                        return -1;
                    }
                    acc = 0;
                }
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

    if (acc > 0) {
        int fq = queue_pcm(api, pcm8, acc, rate);
        if (fq == 1) {
            return 0;
        }
        if (fq != 0) {
            ui_set_status(api, "Playback failed", FG_ERR);
            ui_wait_key(api);
            return -1;
        }
    }
    if (finish_playback(api)) {
        return 0;
    }
    {
        uint32_t pos_ms = rate ? (uint32_t)((played * 1000ull) / rate) : 0;
        ui_progress(api, payload, payload, pos_ms, pos_ms);
    }
    ui_set_status(api, "Done", FG_OK);
    return 0;
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
        ui_set_status(api, "usage: play FILE.WAV  or  play FILE.MP3", FG_MUTED);
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
    if (looks_mp3(sniff, got)) {
        play_mp3(api, path, size);
        goto done;
    }

    if (api->show_error) {
        api->show_error("Not WAV or MP3");
    } else {
        ui_set_status(api, "Not WAV or MP3", FG_ERR);
        ui_wait_key(api);
    }

done:
    if (api->end_direct) {
        api->end_direct();
    }
}
