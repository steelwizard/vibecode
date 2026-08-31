/*
 * tetris.c — Playable tetromino game for the FOS console.
 *
 *   tetris
 *
 * Arrows / WASD move, Up/x rotate CW, z/y rotate CCW, space hard drop,
 * Down soft drop, c hold, p pause, q quit. Click the well to rotate,
 * the sides to nudge, or the bottom bar.
 */

#include "fos_api.h"

#define WELL_W     10
#define WELL_H     20
#define HIDDEN     0
#define BOARD_H    (WELL_H + HIDDEN)
#define CELL_W     2
#define NEXT_N     3
#define LOCK_MS    500
#define LOCK_MAX   15
#define FLASH_MS   90
#define HI_PATH    "\\GAMES\\SCORE.TXT"

#define ST_TITLE   0
#define ST_PLAY    1
#define ST_PAUSE   2
#define ST_OVER    3

#define C_DESK     1
#define C_DESK_FG  9
#define C_WELL     0
#define C_FRAME   15
#define C_FRAME_D  8
#define C_TITLE   14
#define C_MUTED    8
#define C_TEXT    15
#define C_ACC     11
#define C_HOT     12
#define C_CARD     0
#define C_GRID     8
#define C_GHOST    8
#define C_FLASH   15
#define C_BAR      8
#define C_BTN     14

#define CH_TL   0xC9u
#define CH_TR   0xBBu
#define CH_BL   0xC8u
#define CH_BR   0xBCu
#define CH_H    0xCDu
#define CH_V    0xBAu
#define CH_BLK  0xDBu
#define CH_MED  0xB1u
#define CH_DOT  0xFAu

static const uint8_t PCOL[7] = {11, 14, 13, 10, 12, 9, 6};

/* 4 rotations × 4 cells × (x, y) in a 4×4 box. */
static const int8_t SHAPE[7][4][4][2] = {
    /* I */ {
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
        {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
        {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
    },
    /* O */ {
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
    },
    /* T */ {
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    /* S */ {
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
        {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    /* Z */ {
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
        {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
    },
    /* J */ {
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
    },
    /* L */ {
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    },
};

static fos_api_t *api;
static int cols;
static int rows;
static int ox;
static int oy;
static int sx;
static int state;
static int quit;

static uint8_t grid[BOARD_H][WELL_W];
static int kind;
static int rot;
static int px;
static int py;
static int hold;
static int hold_used;
static int bag[7];
static int bag_n;
static int nextp[NEXT_N];
static uint32_t score;
static uint32_t high;
static int lines;
static int level;
static uint32_t rng;
static uint64_t fall_at;
static uint64_t lock_at;
static int lock_n;
static int grounded;
static int btn_l;
static int btn_r;
static int btn_rot;
static int btn_drop;
static int btn_hold;
static int btn_pause;
static int btn_quit;
static int high_dirty;
static int hide_live;

static void draw_play(void);

static uint64_t now_ms(void) {
    return api->get_ticks_ms ? api->get_ticks_ms() : 0;
}

static void beep(uint32_t hz, uint32_t ms) {
    if (api->sound_present && api->sound_present() && api->sound_beep) {
        api->sound_beep(hz, ms);
    }
}

static uint32_t rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fffu;
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
    buf[n] = 0;
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

static void load_high(void) {
    char buf[32];
    uint32_t n = 0;
    int fd;

    high = 0;
    if (!api->fopen) {
        return;
    }
    fd = api->fopen(HI_PATH, FOS_O_READ);
    if (fd < 0) {
        return;
    }
    if (api->fread(fd, buf, (uint32_t)sizeof(buf) - 1, &n) != 0) {
        api->fclose(fd);
        return;
    }
    api->fclose(fd);
    buf[n] = 0;
    high = parse_u32(buf);
}

static void save_high(void) {
    char buf[16];
    uint32_t t = high;
    int n = 0;
    int k = 0;
    char tmp[12];

    if (!high_dirty || !api->fopen) {
        return;
    }
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
    {
        int fd = api->fopen(HI_PATH, FOS_O_WRITE | FOS_O_CREATE | FOS_O_TRUNC);
        uint32_t put = 0;
        if (fd >= 0 && api->fwrite(fd, buf, (uint32_t)n, &put) == 0 && put == (uint32_t)n &&
            api->fclose(fd) == 0) {
            high_dirty = 0;
        } else if (fd >= 0) {
            api->fclose(fd);
        }
    }
}

static int fall_ms(void) {
    int ms = 800 - (level - 1) * 70;
    if (ms < 60) {
        ms = 60;
    }
    return ms;
}

static void piece_cells(int k, int r, int x, int y, int *xs, int *ys) {
    int i;
    for (i = 0; i < 4; i++) {
        xs[i] = x + SHAPE[k][r][i][0];
        ys[i] = y + SHAPE[k][r][i][1];
    }
}

static int fits(int k, int r, int x, int y) {
    int xs[4];
    int ys[4];
    int i;

    piece_cells(k, r, x, y, xs, ys);
    for (i = 0; i < 4; i++) {
        if (xs[i] < 0 || xs[i] >= WELL_W || ys[i] >= BOARD_H) {
            return 0;
        }
        if (ys[i] >= 0 && grid[ys[i]][xs[i]]) {
            return 0;
        }
    }
    return 1;
}

static int is_grounded(void) {
    return !fits(kind, rot, px, py + 1);
}

static void refill_bag(void) {
    int i;

    for (i = 0; i < 7; i++) {
        bag[i] = i;
    }
    for (i = 6; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        int t = bag[i];
        bag[i] = bag[j];
        bag[j] = t;
    }
    bag_n = 7;
}

static int take_piece(void) {
    if (bag_n <= 0) {
        refill_bag();
    }
    return bag[--bag_n];
}

static void touch_lock(void) {
    grounded = is_grounded();
    if (grounded) {
        lock_at = now_ms();
    } else {
        lock_n = 0;
    }
}

static int spawn_kind(int k) {
    kind = k;
    rot = 0;
    px = 3;
    py = 0;
    hold_used = 0;
    lock_n = 0;
    fall_at = now_ms();
    if (!fits(kind, rot, px, py)) {
        return 0;
    }
    touch_lock();
    return 1;
}

static int spawn_next(void) {
    int i;
    int k = nextp[0];
    for (i = 0; i < NEXT_N - 1; i++) {
        nextp[i] = nextp[i + 1];
    }
    nextp[NEXT_N - 1] = take_piece();
    return spawn_kind(k);
}

static void lock_piece(void) {
    int xs[4];
    int ys[4];
    int i;
    int row;
    int cleared = 0;
    uint8_t flash[BOARD_H];

    piece_cells(kind, rot, px, py, xs, ys);
    for (i = 0; i < 4; i++) {
        if (ys[i] >= 0 && ys[i] < BOARD_H) {
            grid[ys[i]][xs[i]] = (uint8_t)(kind + 1);
        }
    }

    for (row = 0; row < BOARD_H; row++) {
        int full = 1;
        int col;
        flash[row] = 0;
        for (col = 0; col < WELL_W; col++) {
            if (!grid[row][col]) {
                full = 0;
                break;
            }
        }
        if (full) {
            flash[row] = 1;
            cleared++;
        }
    }

    if (cleared) {
        int col;
        /* Flash full rows, then collapse. */
        for (row = 0; row < BOARD_H; row++) {
            if (!flash[row]) {
                continue;
            }
            for (col = 0; col < WELL_W; col++) {
                grid[row][col] = 0xFF;
            }
        }
        hide_live = 1;
        draw_play();
        hide_live = 0;
        if (api->sleep_ms) {
            api->sleep_ms(FLASH_MS);
        }
        if (cleared == 4 && api->sleep_ms) {
            api->sleep_ms(FLASH_MS);
        }
        for (row = BOARD_H - 1; row >= 0; row--) {
            if (!flash[row]) {
                continue;
            }
            int r;
            for (r = row; r > 0; r--) {
                for (col = 0; col < WELL_W; col++) {
                    grid[r][col] = grid[r - 1][col];
                }
                flash[r] = flash[r - 1];
            }
            for (col = 0; col < WELL_W; col++) {
                grid[0][col] = 0;
            }
            flash[0] = 0;
            row++;
        }
        if (api->sleep_ms) {
            api->sleep_ms(40);
        }
        lines += cleared;
        if (cleared == 1) {
            score += 100u * (uint32_t)level;
        } else if (cleared == 2) {
            score += 300u * (uint32_t)level;
        } else if (cleared == 3) {
            score += 500u * (uint32_t)level;
        } else {
            score += 800u * (uint32_t)level;
        }
        {
            int nl = lines / 10 + 1;
            if (nl > level) {
                level = nl;
                beep(988, 80);
            }
        }
        if (cleared == 4) {
            beep(880, 120);
        } else {
            beep(660, 50);
        }
    } else {
        beep(196, 25);
    }
    if (score > high) {
        high = score;
        high_dirty = 1;
        save_high();
    }
}

static void new_game(void) {
    int i;
    int j;

    for (i = 0; i < BOARD_H; i++) {
        for (j = 0; j < WELL_W; j++) {
            grid[i][j] = 0;
        }
    }
    score = 0;
    lines = 0;
    level = 1;
    hold = -1;
    hold_used = 0;
    bag_n = 0;
    refill_bag();
    for (i = 0; i < NEXT_N; i++) {
        nextp[i] = take_piece();
    }
    state = ST_PLAY;
    if (!spawn_next()) {
        state = ST_OVER;
    }
}

static int try_move(int dx, int dy) {
    if (!fits(kind, rot, px + dx, py + dy)) {
        return 0;
    }
    px += dx;
    py += dy;
    if (is_grounded()) {
        lock_n++;
        lock_at = now_ms();
    } else {
        lock_n = 0;
    }
    grounded = is_grounded();
    return 1;
}

static int try_rotate(int dir) {
    static const int kx[8] = {0, -1, 1, 0, -1, 1, -2, 2};
    static const int ky[8] = {0, 0, 0, -1, -1, -1, 0, 0};
    int nrot = (rot + dir) & 3;
    int i;

    for (i = 0; i < 8; i++) {
        if (fits(kind, nrot, px + kx[i], py + ky[i])) {
            px += kx[i];
            py += ky[i];
            rot = nrot;
            if (is_grounded()) {
                lock_n++;
                lock_at = now_ms();
            } else {
                lock_n = 0;
            }
            grounded = is_grounded();
            return 1;
        }
    }
    return 0;
}

static void do_hold(void) {
    int k;

    if (state != ST_PLAY || hold_used) {
        return;
    }
    hold_used = 1;
    if (hold < 0) {
        hold = kind;
        if (!spawn_next()) {
            state = ST_OVER;
            beep(110, 220);
        }
        return;
    }
    k = hold;
    hold = kind;
    if (!spawn_kind(k)) {
        state = ST_OVER;
        beep(110, 220);
    }
}

static void hard_drop(void) {
    int n = 0;

    while (fits(kind, rot, px, py + 1)) {
        py++;
        n++;
    }
    if (n) {
        score += 2u * (uint32_t)n;
    }
    lock_piece();
    if (!spawn_next()) {
        state = ST_OVER;
        beep(110, 220);
        save_high();
    }
}

static void soft_drop(void) {
    if (try_move(0, 1)) {
        score += 1;
        fall_at = now_ms();
        return;
    }
    grounded = 1;
    lock_at = now_ms();
}

static int ghost_y(void) {
    int y = py;
    while (fits(kind, rot, px, y + 1)) {
        y++;
    }
    return y;
}

static void layout(void) {
    int well_px = WELL_W * CELL_W + 2;
    int need = well_px + 18;

    cols = 80;
    rows = 25;
    if (api->get_term_size) {
        api->get_term_size(&cols, &rows);
    }
    if (cols < 40) {
        cols = 40;
    }
    if (rows < 22) {
        rows = 22;
    }
    ox = (cols - need) / 2;
    if (ox < 1) {
        ox = 1;
    }
    oy = (rows - WELL_H - 3) / 2;
    if (oy < 1) {
        oy = 1;
    }
    sx = ox + well_px + 2;
}

static void mino(int bx, int by, uint8_t col, int ghost) {
    unsigned char ch = ghost ? CH_MED : CH_BLK;
    uint8_t fg = ghost ? col : 15;
    uint8_t bg = ghost ? C_WELL : col;
    int x = ox + 1 + bx * CELL_W;
    int y = oy + 1 + (by - HIDDEN);

    if (by < HIDDEN) {
        return;
    }
    cell(x, y, fg, bg, ch);
    cell(x + 1, y, fg, bg, ch);
}

static void draw_piece_at(int k, int r, int x0, int y0, int ghost) {
    int xs[4];
    int ys[4];
    int i;
    int minx = 4;
    int miny = 4;

    piece_cells(k, r, 0, 0, xs, ys);
    for (i = 0; i < 4; i++) {
        if (xs[i] < minx) {
            minx = xs[i];
        }
        if (ys[i] < miny) {
            miny = ys[i];
        }
    }
    for (i = 0; i < 4; i++) {
        int x = x0 + (xs[i] - minx) * CELL_W;
        int y = y0 + (ys[i] - miny);
        unsigned char ch = ghost ? CH_MED : CH_BLK;
        uint8_t col = PCOL[k];
        cell(x, y, ghost ? col : 15, ghost ? C_CARD : col, ch);
        cell(x + 1, y, ghost ? col : 15, ghost ? C_CARD : col, ch);
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

static void draw_play(void) {
    int x;
    int y;
    int gy;
    int xs[4];
    int ys[4];
    int i;
    const char *ov = 0;

    fill_rect(0, 0, cols, rows, C_DESK_FG, C_DESK, ' ');
    put_str(ox, oy - 1, C_TITLE, C_DESK, "TETRIS");
    put_str(ox + 8, oy - 1, C_MUTED, C_DESK, "q quit");

    box(ox, oy, WELL_W * CELL_W + 2, WELL_H + 2, C_FRAME, C_WELL);
    for (y = 0; y < WELL_H; y++) {
        for (x = 0; x < WELL_W; x++) {
            uint8_t g = grid[y + HIDDEN][x];
            int cx = ox + 1 + x * CELL_W;
            int cy = oy + 1 + y;
            if (g == 0xFF) {
                cell(cx, cy, 0, C_FLASH, ' ');
                cell(cx + 1, cy, 0, C_FLASH, ' ');
            } else if (g) {
                mino(x, y + HIDDEN, PCOL[g - 1], 0);
            } else {
                cell(cx, cy, C_GRID, C_WELL, CH_DOT);
                cell(cx + 1, cy, C_GRID, C_WELL, ' ');
            }
        }
    }

    if (state != ST_OVER && !hide_live) {
        gy = ghost_y();
        if (gy != py) {
            piece_cells(kind, rot, px, gy, xs, ys);
            for (i = 0; i < 4; i++) {
                mino(xs[i], ys[i], PCOL[kind], 1);
            }
        }
        piece_cells(kind, rot, px, py, xs, ys);
        for (i = 0; i < 4; i++) {
            mino(xs[i], ys[i], PCOL[kind], 0);
        }
    }

    box(sx, oy, 16, 8, C_FRAME_D, C_CARD);
    put_str(sx + 2, oy, C_ACC, C_CARD, " NEXT ");
    for (i = 0; i < NEXT_N; i++) {
        draw_piece_at(nextp[i], 0, sx + 4, oy + 1 + i * 2, 0);
    }

    box(sx, oy + 9, 16, 6, C_FRAME_D, C_CARD);
    put_str(sx + 2, oy + 9, C_ACC, C_CARD, " HOLD ");
    if (hold >= 0) {
        draw_piece_at(hold, 0, sx + 4, oy + 11, hold_used);
    } else {
        put_str(sx + 4, oy + 12, C_MUTED, C_CARD, "c to hold");
    }

    box(sx, oy + 16, 16, 6, C_FRAME_D, C_CARD);
    put_str(sx + 2, oy + 16, C_ACC, C_CARD, " SCORE ");
    put_u32(sx + 2, oy + 17, C_TITLE, C_CARD, score, 12);
    put_str(sx + 2, oy + 18, C_MUTED, C_CARD, "HI");
    put_u32(sx + 5, oy + 18, C_TEXT, C_CARD, high, 9);
    put_str(sx + 2, oy + 19, C_MUTED, C_CARD, "Lv");
    put_u32(sx + 5, oy + 19, C_TEXT, C_CARD, (uint32_t)level, 3);
    put_str(sx + 9, oy + 19, C_MUTED, C_CARD, "Ln");
    put_u32(sx + 12, oy + 19, C_TEXT, C_CARD, (uint32_t)lines, 3);

    if (oy + 23 < rows - 1) {
        put_str(sx, oy + 23, C_MUTED, C_DESK, "z/y rot  x/up");
        put_str(sx, oy + 24, C_MUTED, C_DESK, "space drop  c");
    }

    fill_span(0, rows - 1, cols, C_TEXT, C_BAR, ' ');
    {
        int b = 2;
        btn_l = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "< ");
        b += 3;
        btn_r = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "> ");
        b += 3;
        btn_rot = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "Rot ");
        b += 5;
        btn_drop = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "Drop ");
        b += 6;
        btn_hold = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "Hold ");
        b += 6;
        btn_pause = b;
        put_str(b, rows - 1, C_BTN, C_BAR, "Pause ");
        b += 7;
        btn_quit = b;
        put_str(b, rows - 1, C_HOT, C_BAR, "Quit");
    }

    if (state == ST_PAUSE) {
        ov = " PAUSED — p resume ";
    } else if (state == ST_OVER) {
        ov = " GAME OVER — Enter again, q quit ";
    }
    if (ov) {
        int n = 0;
        int tx;
        while (ov[n]) {
            n++;
        }
        tx = ox + 1;
        if (n < WELL_W * CELL_W) {
            tx = ox + 1 + (WELL_W * CELL_W - n) / 2;
        }
        put_str(tx, oy + WELL_H / 2, 0, C_TITLE, ov);
    }

    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

static void draw_title(void) {
    static const char *logo[5] = {
        "##### ##### ##### ###  #  ###",
        "  #   #       #   #  # # #   ",
        "  #   ####    #   ###  #  ## ",
        "  #   #       #   # #  #    #",
        "  #   #####   #   #  # # ### ",
    };
    static const uint8_t lc[5] = {11, 14, 13, 10, 12};
    int i;
    int j;
    int lx;
    int ly;

    fill_rect(0, 0, cols, rows, C_DESK_FG, C_DESK, ' ');
    lx = (cols - 29) / 2;
    if (lx < 0) {
        lx = 0;
    }
    ly = rows / 2 - 8;
    if (ly < 1) {
        ly = 1;
    }
    for (i = 0; i < 5; i++) {
        for (j = 0; logo[i][j]; j++) {
            if (logo[i][j] == '#') {
                cell(lx + j, ly + i, 15, lc[i], CH_BLK);
            }
        }
    }
    put_str((cols - 28) / 2, ly + 7, C_TEXT, C_DESK, "arrows move    z/y  x rotate");
    put_str((cols - 28) / 2, ly + 8, C_TEXT, C_DESK, "space drop     c hold   p pause");
    put_str((cols - 22) / 2, ly + 10, C_TITLE, C_DESK, "Enter or click to play");
    put_str((cols - 12) / 2, ly + 12, C_MUTED, C_DESK, "q to leave");
    if (high) {
        put_str((cols - 16) / 2, ly + 14, C_ACC, C_DESK, "best");
        put_u32((cols - 16) / 2 + 5, ly + 14, C_TITLE, C_DESK, high, 8);
    }
    fill_span(0, rows - 1, cols, C_TEXT, C_BAR, ' ');
    put_str(2, rows - 1, C_BTN, C_BAR, "Play");
    put_str(8, rows - 1, C_HOT, C_BAR, "Quit");
    if (api->set_cursor_visible) {
        api->set_cursor_visible(0);
    }
}

static int tick_play(void) {
    uint64_t t = now_ms();

    if (state != ST_PLAY) {
        return 0;
    }
    grounded = is_grounded();
    if (!grounded && t >= fall_at + (uint64_t)fall_ms()) {
        if (!try_move(0, 1)) {
            grounded = 1;
            lock_at = t;
        }
        fall_at = t;
        return 1;
    }
    if (grounded && (t >= lock_at + LOCK_MS || lock_n >= LOCK_MAX)) {
        lock_piece();
        if (!spawn_next()) {
            state = ST_OVER;
            beep(110, 220);
            save_high();
        }
        return 1;
    }
    return 0;
}

static void on_quit(void) {
    save_high();
    quit = 1;
}

static void handle_key(fos_key_event_t ev) {
    int ch;

    if (ev.type == FOS_KEY_NONE) {
        return;
    }
    if (ev.type == FOS_KEY_CHAR && (ev.ch == 'q' || ev.ch == 'Q' || ev.ch == 3)) {
        on_quit();
        return;
    }
    if (state == ST_TITLE) {
        if (ev.type != FOS_KEY_NONE) {
            new_game();
        }
        return;
    }
    if (state == ST_OVER) {
        if (ev.type == FOS_KEY_ENTER) {
            new_game();
        }
        return;
    }
    if (state == ST_PAUSE) {
        if (ev.type == FOS_KEY_CHAR && (ev.ch == 'p' || ev.ch == 'P')) {
            state = ST_PLAY;
            fall_at = now_ms();
            lock_at = now_ms();
        }
        return;
    }
    if (ev.type == FOS_KEY_LEFT) {
        try_move(-1, 0);
        return;
    }
    if (ev.type == FOS_KEY_RIGHT) {
        try_move(1, 0);
        return;
    }
    if (ev.type == FOS_KEY_DOWN) {
        soft_drop();
        return;
    }
    if (ev.type == FOS_KEY_UP) {
        try_rotate(1);
        return;
    }
    if (ev.type != FOS_KEY_CHAR) {
        return;
    }
    ch = ev.ch;
    if (ch == 'a' || ch == 'A') {
        try_move(-1, 0);
    } else if (ch == 'd' || ch == 'D') {
        try_move(1, 0);
    } else if (ch == 's' || ch == 'S') {
        soft_drop();
    } else if (ch == 'x' || ch == 'X' || ch == 'e' || ch == 'E') {
        try_rotate(1);
    } else if (ch == 'z' || ch == 'Z' || ch == 'y' || ch == 'Y' || ch == 'w' ||
               ch == 'W') {
        try_rotate(-1);
    } else if (ch == ' ') {
        hard_drop();
    } else if (ch == 'c' || ch == 'C') {
        do_hold();
    } else if (ch == 'p' || ch == 'P') {
        state = ST_PAUSE;
    }
}

static int in_well(int x, int y) {
    return x >= ox + 1 && x < ox + 1 + WELL_W * CELL_W &&
           y >= oy + 1 && y < oy + 1 + WELL_H;
}

static int handle_mouse(void) {
    fos_mouse_t m;
    int acted = 0;

    if (!api->mouse_poll || !api->mouse_poll(&m)) {
        return 0;
    }
    if (!(m.pending & 3)) {
        return 0;
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
        } else if (state == ST_OVER) {
            new_game();
        } else if (m.x >= btn_pause && m.x < btn_pause + 5) {
            if (state == ST_PAUSE) {
                state = ST_PLAY;
                fall_at = now_ms();
            } else if (state == ST_PLAY) {
                state = ST_PAUSE;
            }
        } else if (state == ST_PLAY) {
            if (m.x >= btn_l && m.x < btn_l + 2) {
                try_move(-1, 0);
            } else if (m.x >= btn_r && m.x < btn_r + 2) {
                try_move(1, 0);
            } else if (m.x >= btn_rot && m.x < btn_rot + 3) {
                try_rotate((m.pending & 2) ? -1 : 1);
            } else if (m.x >= btn_drop && m.x < btn_drop + 4) {
                hard_drop();
            } else if (m.x >= btn_hold && m.x < btn_hold + 4) {
                do_hold();
            }
        }
        return 1;
    }
    if (state == ST_OVER && (m.pending & 1)) {
        new_game();
        return 1;
    }
    if (state != ST_PLAY) {
        return 0;
    }
    if (in_well(m.x, m.y)) {
        try_rotate((m.pending & 2) ? -1 : 1);
        return 1;
    }
    if (m.x < ox + 1) {
        acted = try_move(-1, 0);
    } else if (m.x >= ox + 1 + WELL_W * CELL_W) {
        acted = try_move(1, 0);
    }
    return acted;
}

void com_main(void) {
    api = (fos_api_t *)FOS_API_ADDR;
    quit = 0;
    high_dirty = 0;
    hold = -1;

    if (api->begin_direct) {
        api->begin_direct();
    }
    layout();
    rng = (uint32_t)now_ms() ^ 0xA5A5u;
    if (rng == 0) {
        rng = 1;
    }
    load_high();
    state = ST_TITLE;
    if (api->clear_screen) {
        api->clear_screen();
    }
    draw_title();

    while (!quit) {
        int dirty = 0;
        int before;

        if (handle_mouse()) {
            dirty = 1;
        }
        while (api->has_key && api->has_key()) {
            handle_key(api->read_key());
            dirty = 1;
        }
        before = state;
        if (tick_play()) {
            dirty = 1;
        }
        if (state != before) {
            dirty = 1;
        }
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

    save_high();
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
