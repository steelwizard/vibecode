/*
 * irq.c — 8259 PIC, IDT vectors 32–47, and IRQ dispatch for kernel + .COM handlers.
 */

#include "irq.h"
#include "timer.h"
#include "string.h"
#include "console.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define CODE64_SEL 0x18
#define IDT_IRQ0   32

extern void irq_stub_0(void);

/* Called from per-vector exception stubs. Never returns. */
void exception_panic(uint64_t vector, uint64_t err, uint64_t rip, uint64_t cr2) {
    static const char *names[32] = {
        "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
        "#DF", "CSO", "#TS", "#NP", "#SS", "#GP", "#PF", "res",
        "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", "res", "res",
        "res", "res", "res", "res", "res", "res", "#SX", "res"
    };
    console_set_color(15, 4);
    console_write_line("");
    console_write("*** CPU EXCEPTION ");
    if (vector < 32) {
        console_write(names[vector]);
    }
    console_write(" vec=");
    console_write_dec(vector);
    console_write(" err=");
    console_write_hex64(err);
    console_write_line("");
    console_write("RIP=");
    console_write_hex64(rip);
    console_write("  CR2=");
    console_write_hex64(cr2);
    console_write_line("");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}
extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern void irq_stub_2(void);
extern void irq_stub_3(void);
extern void irq_stub_4(void);
extern void irq_stub_5(void);
extern void irq_stub_6(void);
extern void irq_stub_7(void);
extern void irq_stub_8(void);
extern void irq_stub_9(void);
extern void irq_stub_10(void);
extern void irq_stub_11(void);
extern void irq_stub_12(void);
extern void irq_stub_13(void);
extern void irq_stub_14(void);
extern void irq_stub_15(void);

extern void exc_stub_0(void);
extern void exc_stub_1(void);
extern void exc_stub_2(void);
extern void exc_stub_3(void);
extern void exc_stub_4(void);
extern void exc_stub_5(void);
extern void exc_stub_6(void);
extern void exc_stub_7(void);
extern void exc_stub_8(void);
extern void exc_stub_9(void);
extern void exc_stub_10(void);
extern void exc_stub_11(void);
extern void exc_stub_12(void);
extern void exc_stub_13(void);
extern void exc_stub_14(void);
extern void exc_stub_15(void);
extern void exc_stub_16(void);
extern void exc_stub_17(void);
extern void exc_stub_18(void);
extern void exc_stub_19(void);
extern void exc_stub_20(void);
extern void exc_stub_21(void);
extern void exc_stub_22(void);
extern void exc_stub_23(void);
extern void exc_stub_24(void);
extern void exc_stub_25(void);
extern void exc_stub_26(void);
extern void exc_stub_27(void);
extern void exc_stub_28(void);
extern void exc_stub_29(void);
extern void exc_stub_30(void);
extern void exc_stub_31(void);

static void (*const irq_stubs[IRQ_COUNT])(void) = {
    irq_stub_0, irq_stub_1, irq_stub_2, irq_stub_3,
    irq_stub_4, irq_stub_5, irq_stub_6, irq_stub_7,
    irq_stub_8, irq_stub_9, irq_stub_10, irq_stub_11,
    irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15
};

static void (*const exc_stubs[32])(void) = {
    exc_stub_0, exc_stub_1, exc_stub_2, exc_stub_3,
    exc_stub_4, exc_stub_5, exc_stub_6, exc_stub_7,
    exc_stub_8, exc_stub_9, exc_stub_10, exc_stub_11,
    exc_stub_12, exc_stub_13, exc_stub_14, exc_stub_15,
    exc_stub_16, exc_stub_17, exc_stub_18, exc_stub_19,
    exc_stub_20, exc_stub_21, exc_stub_22, exc_stub_23,
    exc_stub_24, exc_stub_25, exc_stub_26, exc_stub_27,
    exc_stub_28, exc_stub_29, exc_stub_30, exc_stub_31
};

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static irq_handler_fn_t handlers[IRQ_COUNT];
static volatile uint32_t pending;
static int in_handler;
static uint8_t master_mask = 0xFE; /* IRQ0 (timer) always unmasked */
static uint8_t slave_mask = 0xFF;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void idt_set(int vec, void (*handler)(void), uint8_t flags) {
    uint64_t addr = (uint64_t)(uintptr_t)handler;

    idt[vec].offset_low = (uint16_t)addr;
    idt[vec].selector = CODE64_SEL;
    idt[vec].ist = 0;
    idt[vec].flags = flags;
    idt[vec].offset_mid = (uint16_t)(addr >> 16);
    idt[vec].offset_high = (uint32_t)(addr >> 32);
    idt[vec].reserved = 0;
}

static void pic_write_masks(void) {
    outb(PIC1_DATA, master_mask);
    outb(PIC2_DATA, slave_mask);
}

static void pic_remap(void) {
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    pic_write_masks();
}

static void pic_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

static void pic_set_masked(uint8_t irq, int masked) {
    if (irq >= IRQ_COUNT) {
        return;
    }
    if (irq == FOS_IRQ_TIMER) {
        return;
    }
    if (irq < 8) {
        if (masked) {
            master_mask = (uint8_t)(master_mask | (uint8_t)(1u << irq));
        } else {
            master_mask = (uint8_t)(master_mask & ~(uint8_t)(1u << irq));
        }
    } else {
        uint8_t bit = (uint8_t)(1u << (irq - 8));
        if (masked) {
            slave_mask = (uint8_t)(slave_mask | bit);
        } else {
            slave_mask = (uint8_t)(slave_mask & ~bit);
        }
    }
    pic_write_masks();
}

void irq_init(void) {
    struct idtr idtr;
    int i;

    memset(handlers, 0, sizeof(handlers));
    pending = 0;
    in_handler = 0;

    for (i = 0; i < 256; i++) {
        idt_set(i, exc_stub_31, 0x8E); /* unused vectors share a reserved stub */
    }
    for (i = 0; i < 32; i++) {
        idt_set(i, exc_stubs[i], 0x8E);
    }
    for (i = 0; i < IRQ_COUNT; i++) {
        idt_set(IDT_IRQ0 + i, irq_stubs[i], 0x8E);
    }

    idtr.limit = (uint16_t)(sizeof(idt) - 1);
    idtr.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));

    /* Programs use SSE (minimp3, and any float math under the SysV ABI), which
     * #UDs without OSFXSR. The kernel itself is built -mno-sse, so IRQ handlers
     * never touch XMM and the stubs don't need to save it. */
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ull << 2); /* EM */
        cr0 |= (1ull << 1);  /* MP */
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ull << 9) | (1ull << 10); /* OSFXSR | OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));
        __asm__ volatile("fninit");
        {
            static uint32_t mxcsr = 0x1F80u; /* default: all SSE exceptions masked */
            __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
        }
    }

    pic_remap();
    __asm__ volatile("sti");
}

void irq_dispatch(uint8_t irq) {
    irq_handler_fn_t fn;

    if (irq >= IRQ_COUNT) {
        return;
    }

    pic_eoi(irq);

    if (irq == FOS_IRQ_TIMER) {
        timer_on_irq();
    }

    pending = (uint32_t)(pending | (1u << irq));

    fn = handlers[irq];
    if (!fn) {
        return;
    }

    in_handler = 1;
    fn(irq);
    in_handler = 0;
}

int irq_register(uint8_t irq, irq_handler_fn_t handler) {
    if (irq >= IRQ_COUNT || !handler) {
        return -1;
    }
    handlers[irq] = handler;
    return 0;
}

int irq_unregister(uint8_t irq) {
    if (irq >= IRQ_COUNT) {
        return -1;
    }
    handlers[irq] = 0;
    return 0;
}

void irq_enable(uint8_t irq) {
    /* Slave PIC IRQs never fire unless cascade IRQ2 is unmasked. */
    if (irq >= 8 && irq < IRQ_COUNT) {
        pic_set_masked(FOS_IRQ_CASCADE, 0);
    }
    pic_set_masked(irq, 0);
}

void irq_disable(uint8_t irq) {
    pic_set_masked(irq, 1);
}

uint32_t irq_get_pending(void) {
    return pending;
}

void irq_clear_pending(uint8_t irq) {
    if (irq < IRQ_COUNT) {
        pending = (uint32_t)(pending & ~(1u << irq));
    }
}

int irq_in_handler(void) {
    return in_handler;
}

void irq_clear_handlers(void) {
    memset(handlers, 0, sizeof(handlers));
    pending = 0;
    master_mask = 0xFE;
    slave_mask = 0xFF;
    pic_write_masks();
}
