/*
 * timer.c — 8253/8254 PIT at 1000 Hz (IRQ0).
 *
 * IDT/PIC setup lives in irq.c. Keyboard stays polled (IRQ1 masked).
 */

#include "timer.h"
#include "irq.h"
#include "types.h"

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_HZ    1193182u
#define TICK_HZ   1000u

static volatile uint64_t ticks_ms;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void pit_start(void) {
    uint32_t div = PIT_HZ / TICK_HZ;

    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)div);
    outb(PIT_CH0, (uint8_t)(div >> 8));
}

void timer_on_irq(void) {
    ticks_ms++;
}

void timer_init(void) {
    ticks_ms = 0;
    irq_init();
    pit_start();
}

uint64_t timer_ticks_ms(void) {
    return ticks_ms;
}

void timer_sleep_ms(uint32_t ms) {
    uint64_t until;

    if (ms == 0) {
        return;
    }
    until = ticks_ms + ms;
    __asm__ volatile("sti");
    while (ticks_ms < until) {
        __asm__ volatile("hlt");
    }
}
