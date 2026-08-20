/*
 * sb16.c — Creative Sound Blaster 16 (ISA): DSP reset, mixer, 8-bit DMA playback.
 *
 * QEMU: -device sb16,audiodev=snd0  (iobase=0x220 irq=5 dma=1)
 * Playback is 8-bit unsigned mono through DMA channel 1. The DMA buffer sits
 * at 0x20000 so it is 64K-aligned and below the ISA 16 MiB limit. Auto-init
 * uses the whole 64 KiB page as two 32 KiB halves (~0.74 s at 44.1 kHz).
 */

#include "sb16.h"
#include "irq.h"
#include "timer.h"
#include "config.h"
#include "console.h"
#include "string.h"

#define DSP_RESET     0x06
#define DSP_READ      0x0A
#define DSP_WRITE     0x0C
#define DSP_STATUS    0x0E
#define DSP_ACK16     0x0F
#define MIX_INDEX     0x04
#define MIX_DATA      0x05

#define DMA_MASK      0x0A
#define DMA_CLEAR     0x0C
#define DMA_MODE      0x0B
#define DMA_CH1_ADDR  0x02
#define DMA_CH1_COUNT 0x03
#define DMA_CH1_PAGE  0x83

#define DMA_BUF_ADDR  0x20000ULL
/* 8-bit ISA DMA cannot cross a 64 KiB page. 0x20000..0x2FFFF is one page. */
#define DMA_HALF      32768u
#define DMA_FULL      (DMA_HALF * 2u)
#define BEEP_CHUNK    4096u
#define BEEP_RATE     8000u

#define DSP_CMD_SPEAKER_ON  0xD1
#define DSP_CMD_SPEAKER_OFF 0xD3
#define DSP_CMD_HALT8       0xD0
#define DSP_CMD_EXIT_AI8    0xDA
#define DSP_CMD_GETVER      0xE1
#define DSP_CMD_TIMECONST   0x40
#define DSP_CMD_SETRATE     0x41
#define DSP_CMD_DMA8        0x14
#define DSP_CMD_DMA8_SB16   0xC0
#define DSP_CMD_DMA8_AI     0xC6

static uint16_t sb_base;
static uint8_t  sb_irq = 5;
static uint8_t  sb_dma = 1;
static uint8_t  dsp_major;
static uint8_t  dsp_minor;
static int      found;
static volatile int dma_done;
static volatile int playing;
static volatile int half_filled[2];
static volatile int play_half;
static volatile int auto_stream;
static int queued_half;
static int primed;
static int underrun_streak;
static uint32_t play_rate;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_delay(void) {
    outb(0x80, 0);
}

static int dsp_write(uint8_t value) {
    int i;
    for (i = 0; i < 100000; i++) {
        if ((inb((uint16_t)(sb_base + DSP_WRITE)) & 0x80) == 0) {
            outb((uint16_t)(sb_base + DSP_WRITE), value);
            return 0;
        }
    }
    return -1;
}

static int dsp_read(uint8_t *out) {
    int i;
    for (i = 0; i < 100000; i++) {
        if (inb((uint16_t)(sb_base + DSP_STATUS)) & 0x80) {
            *out = inb((uint16_t)(sb_base + DSP_READ));
            return 0;
        }
    }
    return -1;
}

static void mix_write(uint8_t index, uint8_t value) {
    outb((uint16_t)(sb_base + MIX_INDEX), index);
    io_delay();
    outb((uint16_t)(sb_base + MIX_DATA), value);
}

static void dsp_ack(void) {
    (void)inb((uint16_t)(sb_base + DSP_STATUS));
}

static void half_complete(void);

static int dsp_reset(void) {
    uint8_t data = 0;
    int i;

    outb((uint16_t)(sb_base + DSP_RESET), 1);
    for (i = 0; i < 100; i++) {
        io_delay();
    }
    outb((uint16_t)(sb_base + DSP_RESET), 0);

    for (i = 0; i < 10000; i++) {
        if (dsp_read(&data) == 0 && data == 0xAA) {
            return 0;
        }
    }
    return -1;
}

static void on_irq(uint8_t irq) {
    (void)irq;
    dsp_ack();
    if (auto_stream) {
        half_complete();
    } else {
        dma_done = 1;
        playing = 0;
    }
}

static void dma_mask(int masked) {
    /* Channel 1: bit0–1 = 1, bit2 = mask. */
    outb(DMA_MASK, (uint8_t)(masked ? 0x05 : 0x01));
}

static void dma_program(uint32_t phys, uint32_t length, int auto_init) {
    uint32_t count = length - 1;

    dma_mask(1);
    outb(DMA_CLEAR, 0);
    /* Single-cycle 0x49 / auto-init 0x59: increment, read memory, channel 1. */
    outb(DMA_MODE, (uint8_t)(auto_init ? 0x59 : 0x49));
    outb(DMA_CH1_ADDR, (uint8_t)(phys & 0xFF));
    outb(DMA_CH1_ADDR, (uint8_t)((phys >> 8) & 0xFF));
    outb(DMA_CH1_PAGE, (uint8_t)((phys >> 16) & 0xFF));
    outb(DMA_CH1_COUNT, (uint8_t)(count & 0xFF));
    outb(DMA_CH1_COUNT, (uint8_t)((count >> 8) & 0xFF));
    dma_mask(0);
}

static int dsp_start_output(uint32_t length, uint32_t rate, int auto_init) {
    uint32_t n = length - 1;
    uint8_t cmd;

    if (dsp_write(DSP_CMD_SPEAKER_ON) != 0) {
        return -1;
    }

    if (dsp_major >= 4) {
        if (dsp_write(DSP_CMD_SETRATE) != 0 ||
            dsp_write((uint8_t)((rate >> 8) & 0xFF)) != 0 ||
            dsp_write((uint8_t)(rate & 0xFF)) != 0) {
            return -1;
        }
        cmd = auto_init ? DSP_CMD_DMA8_AI : DSP_CMD_DMA8_SB16;
        if (dsp_write(cmd) != 0 ||
            dsp_write(0x00) != 0 ||
            dsp_write((uint8_t)(n & 0xFF)) != 0 ||
            dsp_write((uint8_t)((n >> 8) & 0xFF)) != 0) {
            return -1;
        }
    } else {
        uint32_t tc = 256u - (1000000u / rate);
        if (tc > 255) {
            tc = 255;
        }
        if (dsp_write(DSP_CMD_TIMECONST) != 0 ||
            dsp_write((uint8_t)tc) != 0) {
            return -1;
        }
        if (dsp_write(DSP_CMD_DMA8) != 0 ||
            dsp_write((uint8_t)(n & 0xFF)) != 0 ||
            dsp_write((uint8_t)((n >> 8) & 0xFF)) != 0) {
            return -1;
        }
    }
    return 0;
}

static void silence_dma(void) {
    memset((void *)(uintptr_t)DMA_BUF_ADDR, 0x80, DMA_FULL);
}

static void silence_half(int half) {
    uint8_t *dst = (uint8_t *)(uintptr_t)DMA_BUF_ADDR + (uint32_t)half * DMA_HALF;
    memset(dst, 0x80, DMA_HALF);
}

static void copy_half(int half, const uint8_t *src, uint32_t n) {
    uint8_t *dst = (uint8_t *)(uintptr_t)DMA_BUF_ADDR + (uint32_t)half * DMA_HALF;
    if (n > DMA_HALF) {
        n = DMA_HALF;
    }
    memcpy(dst, src, (size_t)n);
    if (n < DMA_HALF) {
        memset(dst + n, 0x80, DMA_HALF - n);
    }
}

static void stream_halt(void) {
    auto_stream = 0;
    primed = 0;
    (void)dsp_write(DSP_CMD_EXIT_AI8);
    (void)dsp_write(DSP_CMD_HALT8);
    (void)dsp_write(DSP_CMD_SPEAKER_OFF);
    dma_mask(1);
    dsp_ack();
    playing = 0;
    dma_done = 1;
    half_filled[0] = 0;
    half_filled[1] = 0;
    underrun_streak = 0;
}

static void half_complete(void) {
    int done;
    if (!playing) {
        return;
    }
    done = play_half;
    half_filled[done] = 0;
    play_half ^= 1;
    queued_half = done;
    if (!half_filled[play_half]) {
        /* DMA already wrapped into this half. Play silence instead of
         * stopping — a restart click is worse than a brief dropout. */
        silence_half(play_half);
        half_filled[play_half] = 1;
        underrun_streak++;
        if (underrun_streak >= 2) {
            stream_halt();
        }
        return;
    }
    underrun_streak = 0;
}

static int play_stream_start(uint32_t rate) {
    irq_register(sb_irq, on_irq);
    irq_enable(sb_irq);
    dma_done = 0;
    auto_stream = 1;
    primed = 0;
    playing = 1;
    play_rate = rate;
    play_half = 0;
    underrun_streak = 0;
    dma_program((uint32_t)DMA_BUF_ADDR, DMA_FULL, 1);
    if (dsp_start_output(DMA_HALF, rate, 1) != 0) {
        stream_halt();
        return -1;
    }
    return 0;
}

static void commit_prime(void) {
    if (!primed || playing) {
        return;
    }
    half_filled[1] = 1;
    queued_half = 0;
    primed = 0;
    (void)play_stream_start(play_rate);
}

static int wait_irq_done(uint32_t timeout_ms) {
    uint64_t start = timer_ticks_ms();

    __asm__ volatile("sti");
    while (playing) {
        if (timer_ticks_ms() - start > (uint64_t)timeout_ms) {
            return -1;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

static uint32_t parse_hex(const char *s, uint32_t fallback) {
    uint32_t v = 0;
    int any = 0;

    if (!s || !s[0]) {
        return fallback;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            break;
        }
        v = (v << 4) | d;
        any = 1;
    }
    return any ? v : fallback;
}

static uint32_t parse_dec(const char *s, uint32_t fallback) {
    uint32_t v = 0;
    int any = 0;

    if (!s || !s[0]) {
        return fallback;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
        any = 1;
    }
    return any ? v : fallback;
}

static int try_base(uint16_t base) {
    sb_base = base;
    if (dsp_reset() != 0) {
        return -1;
    }
    if (dsp_write(DSP_CMD_GETVER) != 0 ||
        dsp_read(&dsp_major) != 0 ||
        dsp_read(&dsp_minor) != 0) {
        return -1;
    }
    return 0;
}

void sb16_stop(void) {
    if (!found) {
        return;
    }
    stream_halt();
}

int sb16_present(void) {
    return found;
}

int sb16_can_queue(void) {
    if (!found) {
        return 0;
    }
    if (!playing) {
        return 1;
    }
    return !half_filled[queued_half];
}

int sb16_playing(void) {
    commit_prime();
    return playing;
}

uint32_t sb16_buf_size(void) {
    return DMA_HALF;
}

int sb16_start(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz) {
    int half;

    if (!found || !pcm || bytes == 0) {
        return -1;
    }
    if (rate_hz < 4000) {
        rate_hz = 4000;
    }
    if (rate_hz > 44100) {
        rate_hz = 44100;
    }
    if (bytes > DMA_HALF) {
        bytes = DMA_HALF;
    }

    if (playing && play_rate != rate_hz) {
        stream_halt();
    }

    __asm__ volatile("cli");
    if (!playing && !primed) {
        silence_dma();
        copy_half(0, pcm, bytes);
        half_filled[0] = 1;
        half_filled[1] = 0;
        queued_half = 1;
        play_rate = rate_hz;
        primed = 1;
        __asm__ volatile("sti");
        return 0;
    }
    if (primed && !playing) {
        copy_half(1, pcm, bytes);
        half_filled[1] = 1;
        queued_half = 0;
        primed = 0;
        __asm__ volatile("sti");
        return play_stream_start(play_rate);
    }
    if (half_filled[queued_half]) {
        __asm__ volatile("sti");
        return -1;
    }
    half = queued_half;
    copy_half(half, pcm, bytes);
    half_filled[half] = 1;
    queued_half = half ^ 1;
    underrun_streak = 0;
    __asm__ volatile("sti");
    return 0;
}

int sb16_play(const uint8_t *pcm, uint32_t bytes, uint32_t rate_hz) {
    uint32_t off;

    if (!found || !pcm || bytes == 0) {
        return -1;
    }
    if (rate_hz < 4000) {
        rate_hz = 4000;
    }
    if (rate_hz > 44100) {
        rate_hz = 44100;
    }

    /* One-shot DMA at the true length so a 200 ms beep is not padded to a
     * 32 KiB auto-init half (~4 s of silence at 8 kHz). */
    stream_halt();
    irq_register(sb_irq, on_irq);
    irq_enable(sb_irq);
    auto_stream = 0;

    for (off = 0; off < bytes; ) {
        uint32_t n = bytes - off;
        uint32_t timeout;
        if (n > DMA_FULL) {
            n = DMA_FULL;
        }
        memcpy((void *)(uintptr_t)DMA_BUF_ADDR, pcm + off, (size_t)n);
        dma_done = 0;
        playing = 1;
        dma_program((uint32_t)DMA_BUF_ADDR, n, 0);
        if (dsp_start_output(n, rate_hz, 0) != 0) {
            stream_halt();
            return -1;
        }
        timeout = (n * 1000u) / rate_hz + 200u;
        if (wait_irq_done(timeout) != 0) {
            stream_halt();
            return -1;
        }
        off += n;
    }
    stream_halt();
    return 0;
}

int sb16_beep(uint32_t freq_hz, uint32_t ms) {
    uint8_t buf[BEEP_CHUNK];
    uint32_t samples;
    uint32_t period;
    uint32_t i;
    uint32_t half;

    if (!found) {
        return -1;
    }
    if (freq_hz < 50) {
        freq_hz = 440;
    }
    if (freq_hz > 4000) {
        freq_hz = 4000;
    }
    if (ms < 1) {
        ms = 1;
    }
    if (ms > 5000) {
        ms = 5000;
    }

    samples = (BEEP_RATE * ms) / 1000u;
    if (samples < 8) {
        samples = 8;
    }
    period = BEEP_RATE / freq_hz;
    if (period < 2) {
        period = 2;
    }
    half = period / 2u;

    for (i = 0; i < samples && i < BEEP_CHUNK; i++) {
        buf[i] = ((i / half) & 1u) ? 0xE0 : 0x20;
    }
    if (samples > BEEP_CHUNK) {
        /* Repeat the pattern in chunks for long beeps. */
        uint32_t left = samples;
        while (left) {
            uint32_t n = left > BEEP_CHUNK ? BEEP_CHUNK : left;
            for (i = 0; i < n; i++) {
                buf[i] = (( (samples - left + i) / half) & 1u) ? 0xE0 : 0x20;
            }
            if (sb16_play(buf, n, BEEP_RATE) != 0) {
                return -1;
            }
            left -= n;
        }
        return 0;
    }
    return sb16_play(buf, samples, BEEP_RATE);
}

void sb16_init(void) {
    static const uint16_t bases[] = {0x220, 0x240, 0x260, 0x280};
    const char *cfg;
    unsigned i;

    found = 0;
    playing = 0;
    dma_done = 1;
    auto_stream = 0;
    primed = 0;
    underrun_streak = 0;
    half_filled[0] = 0;
    half_filled[1] = 0;
    play_half = 0;
    queued_half = 0;
    play_rate = 0;
    sb_irq = 5;
    sb_dma = 1;

    cfg = config_get("sound", "irq");
    sb_irq = (uint8_t)parse_dec(cfg, 5);
    if (sb_irq > 15 || sb_irq == 0 || sb_irq == 2) {
        sb_irq = 5;
    }
    cfg = config_get("sound", "dma");
    sb_dma = (uint8_t)parse_dec(cfg, 1);
    (void)sb_dma; /* 8-bit playback is always ISA channel 1 */

    cfg = config_get("sound", "port");
    if (cfg && cfg[0]) {
        uint16_t port = (uint16_t)parse_hex(cfg, 0x220);
        if (try_base(port) == 0) {
            found = 1;
        }
    }
    for (i = 0; !found && i < sizeof(bases) / sizeof(bases[0]); i++) {
        if (try_base(bases[i]) == 0) {
            found = 1;
        }
    }

    if (!found) {
        boot_line("[boot] no Sound Blaster (QEMU: -device sb16)");
        return;
    }

    mix_write(0x00, 0x00);
    mix_write(0x04, 0xFF);
    mix_write(0x22, 0xFF);
    mix_write(0x30, 0xFF);
    mix_write(0x31, 0xFF);
    mix_write(0x32, 0xFF);
    mix_write(0x33, 0xFF);
    /* IRQ5 = 0x02, 8-bit DMA1 = 0x02 */
    mix_write(0x80, (uint8_t)(sb_irq == 7 ? 0x04 : sb_irq == 10 ? 0x08 : 0x02));
    mix_write(0x81, 0x02);

    irq_register(sb_irq, on_irq);
    dsp_ack();

    {
        char msg[72];
        int n = 0;
        const char *p = "[boot] SB16 at ";
        while (*p) {
            msg[n++] = *p++;
        }
        msg[n++] = (char)("0123456789ABCDEF"[(sb_base >> 8) & 0xF]);
        msg[n++] = (char)("0123456789ABCDEF"[(sb_base >> 4) & 0xF]);
        msg[n++] = (char)("0123456789ABCDEF"[sb_base & 0xF]);
        msg[n++] = 'h';
        p = " IRQ";
        while (*p) {
            msg[n++] = *p++;
        }
        if (sb_irq >= 10) {
            msg[n++] = '1';
            msg[n++] = (char)('0' + (sb_irq - 10));
        } else {
            msg[n++] = (char)('0' + sb_irq);
        }
        p = " DMA1 (DSP ";
        while (*p) {
            msg[n++] = *p++;
        }
        msg[n++] = (char)('0' + dsp_major);
        msg[n++] = '.';
        msg[n++] = (char)('0' + (dsp_minor / 10));
        msg[n++] = (char)('0' + (dsp_minor % 10));
        msg[n++] = ')';
        msg[n] = 0;
        boot_line(msg);
    }
}
