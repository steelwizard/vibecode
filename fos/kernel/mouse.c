/*
 * mouse.c — PS/2 relative mouse, plus QEMU/VMware absolute pointer.
 *
 * IRQ12 queues aux bytes. Packet parse and cursor paint run on the main
 * thread. When the VMware backdoor is present (-device vmmouse), the host
 * cursor maps 1:1 onto the guest — no pointer grab.
 */

#include "mouse.h"
#include "console.h"
#include "irq.h"
#include "timer.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

#define SCALE 3
#define QMAX  32

#define VMWARE_MAGIC  0x564D5868u
#define VMWARE_PORT   0x5658u
#define CMD_GETVERSION          10u
#define CMD_ABSPOINTER_DATA     39u
#define CMD_ABSPOINTER_STATUS   40u
#define CMD_ABSPOINTER_COMMAND  41u
#define VMMOUSE_ENABLE     0x45414552u
#define VMMOUSE_DISABLE    0x000000F5u
#define VMMOUSE_ABSOLUTE   0x53424152u
#define VMMOUSE_VERSION_ID 0x3442554Au
#define VMMOUSE_ERROR      0xFFFF0000u
#define VMMOUSE_RELATIVE   0x00010000u
#define VMMOUSE_LEFT_B     0x20u
#define VMMOUSE_RIGHT_B    0x10u
#define VMMOUSE_MIDDLE_B   0x08u

static int aux_on;
static int vmmouse_on;
static int present;
static int pkt_n;
static uint8_t pkt[3];
static int px;
static int py;
static int cell_x;
static int cell_y;
static uint8_t buttons;
static uint8_t pending;
static uint8_t left_held;
static int drag;
static int press_x;
static int press_y;

static volatile uint8_t q[QMAX];
static volatile uint8_t qn;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void cli(void) {
    __asm__ volatile("cli");
}

static inline void sti(void) {
    __asm__ volatile("sti");
}

static void vmport(uint32_t cmd, uint32_t arg,
                   uint32_t *ax, uint32_t *bx, uint32_t *cx, uint32_t *dx) {
    uint32_t a = VMWARE_MAGIC;
    uint32_t b = arg;
    uint32_t c = cmd;
    uint32_t d = VMWARE_PORT;

    __asm__ volatile("inl %%dx, %%eax"
                     : "+a"(a), "+b"(b), "+c"(c), "+d"(d)
                     :
                     : "memory");
    if (ax) {
        *ax = a;
    }
    if (bx) {
        *bx = b;
    }
    if (cx) {
        *cx = c;
    }
    if (dx) {
        *dx = d;
    }
}

static int vmware_present(void) {
    uint32_t ax = 0;
    uint32_t bx = 0;

    vmport(CMD_GETVERSION, 0, &ax, &bx, 0, 0);
    return bx == VMWARE_MAGIC && ax != 0xFFFFFFFFu;
}

static int vmmouse_enable(void) {
    uint32_t ax = 0;
    uint32_t ver = 0;

    if (!vmware_present()) {
        return 0;
    }
    vmport(CMD_ABSPOINTER_COMMAND, VMMOUSE_ENABLE, 0, 0, 0, 0);
    vmport(CMD_ABSPOINTER_STATUS, 0, &ax, 0, 0, 0);
    if ((ax & 0xFFFFu) == 0) {
        return 0;
    }
    vmport(CMD_ABSPOINTER_DATA, 1, &ver, 0, 0, 0);
    if (ver != VMMOUSE_VERSION_ID) {
        vmport(CMD_ABSPOINTER_COMMAND, VMMOUSE_DISABLE, 0, 0, 0, 0);
        return 0;
    }
    vmport(CMD_ABSPOINTER_COMMAND, VMMOUSE_ABSOLUTE, 0, 0, 0, 0);
    return 1;
}

static void wait_write(void) {
    uint64_t start = timer_ticks_ms();
    while (timer_ticks_ms() - start < 50ull) {
        if ((inb(PS2_STATUS) & 0x02) == 0) {
            return;
        }
    }
}

static void write_cmd(uint8_t cmd) {
    wait_write();
    outb(PS2_STATUS, cmd);
}

static void write_data(uint8_t data) {
    wait_write();
    outb(PS2_DATA, data);
}

static void flush_obf(void) {
    int n = 0;
    while (n++ < 64 && (inb(PS2_STATUS) & 0x01)) {
        (void)inb(PS2_DATA);
    }
}

static int read_tagged(uint8_t *out, uint32_t ms, int want_aux) {
    uint64_t start = timer_ticks_ms();
    while (timer_ticks_ms() - start < (uint64_t)ms) {
        uint8_t st = inb(PS2_STATUS);
        if (st & 0x01) {
            uint8_t b = inb(PS2_DATA);
            if (((st & 0x20) != 0) == want_aux) {
                *out = b;
                return 0;
            }
        }
    }
    return -1;
}

static int aux_cmd(uint8_t cmd, uint32_t ms) {
    uint8_t ack = 0;
    write_cmd(0xD4);
    write_data(cmd);
    if (read_tagged(&ack, ms, 1) != 0 || ack != 0xFA) {
        return -1;
    }
    return 0;
}

static void q_push(uint8_t b) {
    if (qn < QMAX) {
        q[qn++] = b;
    }
}

static void hw_drain_into_q(void) {
    int n = 0;
    while (n++ < 16) {
        uint8_t st = inb(PS2_STATUS);
        if ((st & 0x01) == 0 || (st & 0x20) == 0) {
            break;
        }
        q_push(inb(PS2_DATA));
    }
}

static void mouse_on_irq(uint8_t irq) {
    (void)irq;
    hw_drain_into_q();
}

static void clamp_pos(void) {
    int cols = 80;
    int rows = 25;
    int maxx;
    int maxy;

    console_get_size(&cols, &rows);
    if (cols < 1) {
        cols = 80;
    }
    if (rows < 1) {
        rows = 25;
    }
    maxx = cols * 8 - 1;
    maxy = rows * 16 - 1;
    if (px < 0) {
        px = 0;
    }
    if (py < 0) {
        py = 0;
    }
    if (px > maxx) {
        px = maxx;
    }
    if (py > maxy) {
        py = maxy;
    }
    cell_x = px / 8;
    cell_y = py / 16;
}

static void apply_buttons(uint8_t nb, int nx, int ny) {
    if ((nb & MOUSE_LEFT) && !left_held) {
        left_held = 1;
        drag = 0;
        press_x = nx;
        press_y = ny;
        console_mouse_left(1, nx, ny);
    } else if ((nb & MOUSE_LEFT) && left_held) {
        if (nx != press_x || ny != press_y) {
            drag = 1;
        }
        console_mouse_left(2, nx, ny);
    } else if (!(nb & MOUSE_LEFT) && left_held) {
        left_held = 0;
        console_mouse_left(0, nx, ny);
        if (!drag) {
            if (!console_hit_click(nx, ny)) {
                pending |= MOUSE_CLICK_L;
            }
        }
    }

    if ((buttons & MOUSE_RIGHT) && !(nb & MOUSE_RIGHT)) {
        console_mouse_right(nx, ny);
        pending |= MOUSE_CLICK_R;
    }

    buttons = nb;
}

static void apply_pos(uint8_t nb) {
    int nx;
    int ny;

    clamp_pos();
    nx = cell_x;
    ny = cell_y;
    present = 1;
    console_mouse_pixel(px, py);
    apply_buttons(nb, nx, ny);
}

static void apply_packet(void) {
    int8_t dx;
    int8_t dy;
    uint8_t nb;

    if ((pkt[0] & 0x08) == 0) {
        pkt_n = 0;
        return;
    }
    dx = (int8_t)pkt[1];
    dy = (int8_t)pkt[2];
    if (pkt[0] & 0x40 || pkt[0] & 0x80) {
        pkt_n = 0;
        return;
    }
    px += (int)dx * SCALE;
    py -= (int)dy * SCALE;
    nb = (uint8_t)(pkt[0] & 7u);
    apply_pos(nb);
    pkt_n = 0;
}

void mouse_feed(uint8_t b) {
    if (!aux_on || vmmouse_on) {
        return;
    }
    if (pkt_n == 0 && (b & 0x08) == 0) {
        return;
    }
    pkt[pkt_n++] = b;
    if (pkt_n >= 3) {
        apply_packet();
    }
}

static uint8_t vmmouse_to_buttons(uint32_t st) {
    uint8_t nb = 0;
    if (st & VMMOUSE_LEFT_B) {
        nb |= MOUSE_LEFT;
    }
    if (st & VMMOUSE_RIGHT_B) {
        nb |= MOUSE_RIGHT;
    }
    if (st & VMMOUSE_MIDDLE_B) {
        nb |= MOUSE_MIDDLE;
    }
    return nb;
}

static void vmmouse_poll(void) {
    int n = 0;

    while (n++ < 32) {
        uint32_t st = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;
        int cols = 80;
        int rows = 25;
        int sw;
        int sh;

        vmport(CMD_ABSPOINTER_STATUS, 0, &st, 0, 0, 0);
        if ((st & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            vmmouse_on = 0;
            return;
        }
        if ((st & 0xFFFFu) < 4u) {
            return;
        }
        vmport(CMD_ABSPOINTER_DATA, 4, &st, &x, &y, &z);
        console_get_size(&cols, &rows);
        sw = cols * 8;
        sh = rows * 16;
        if (sw < 2) {
            sw = 2;
        }
        if (sh < 2) {
            sh = 2;
        }
        if (st & VMMOUSE_RELATIVE) {
            px += (int)x;
            py -= (int)y;
        } else {
            px = (int)((uint64_t)x * (uint64_t)(sw - 1) / 65535ull);
            py = (int)((uint64_t)y * (uint64_t)(sh - 1) / 65535ull);
        }
        apply_pos(vmmouse_to_buttons(st));
        (void)z;
    }
}

void mouse_poll_hw(void) {
    uint8_t tmp[QMAX];
    uint8_t n;
    uint8_t i;

    if (!aux_on) {
        return;
    }

    if (vmmouse_on) {
        vmmouse_poll();
    }

    cli();
    hw_drain_into_q();
    n = qn;
    for (i = 0; i < n; i++) {
        tmp[i] = q[i];
    }
    qn = 0;
    sti();

    if (vmmouse_on) {
        return;
    }
    for (i = 0; i < n; i++) {
        mouse_feed(tmp[i]);
    }
}

void mouse_irq_restore(void) {
    if (!aux_on) {
        return;
    }
    irq_register(FOS_IRQ_MOUSE, mouse_on_irq);
    irq_enable(FOS_IRQ_MOUSE);
}

void mouse_on_resize(void) {
    if (!aux_on) {
        return;
    }
    clamp_pos();
    console_mouse_pixel(px, py);
}

int mouse_present(void) {
    return present || aux_on;
}

int mouse_is_absolute(void) {
    return vmmouse_on;
}

int mouse_get(mouse_state_t *out) {
    mouse_poll_hw();
    if (!out) {
        return aux_on;
    }
    out->x = (int16_t)cell_x;
    out->y = (int16_t)cell_y;
    out->buttons = buttons;
    out->pending = pending;
    pending = 0;
    return aux_on;
}

void mouse_init(void) {
    uint8_t cfg = 0;
    uint8_t b = 0;

    aux_on = 0;
    vmmouse_on = 0;
    present = 0;
    pkt_n = 0;
    qn = 0;
    buttons = 0;
    pending = 0;
    left_held = 0;
    px = 40 * 8;
    py = 12 * 16;

    flush_obf();
    write_cmd(0xA8);
    write_cmd(0x20);
    if (read_tagged(&cfg, 50, 0) != 0) {
        return;
    }
    cfg &= (uint8_t)~(1u << 5);
    cfg |= (uint8_t)(1u << 1);
    write_cmd(0x60);
    write_data(cfg);

    if (aux_cmd(0xFF, 750) == 0) {
        (void)read_tagged(&b, 750, 1);
        (void)read_tagged(&b, 100, 1);
    }
    (void)aux_cmd(0xF6, 100);
    (void)aux_cmd(0xF4, 200);

    aux_on = 1;
    present = 1;
    vmmouse_on = vmmouse_enable();
    mouse_irq_restore();
    clamp_pos();
    console_mouse_pixel(px, py);
}
