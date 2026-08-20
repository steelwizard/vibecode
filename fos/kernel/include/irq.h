#pragma once

#include "types.h"

#define IRQ_COUNT 16

/* PIC line numbers exposed to .COM programs via fos_api. */
#define FOS_IRQ_TIMER     0
#define FOS_IRQ_KEYBOARD  1
#define FOS_IRQ_CASCADE   2
#define FOS_IRQ_COM2      3
#define FOS_IRQ_COM1      4
#define FOS_IRQ_LPT2      5
#define FOS_IRQ_FLOPPY    6
#define FOS_IRQ_LPT1      7
#define FOS_IRQ_RTC       8
#define FOS_IRQ_FREE9     9
#define FOS_IRQ_FREE10    10
#define FOS_IRQ_FREE11    11
#define FOS_IRQ_MOUSE     12
#define FOS_IRQ_FPU       13
#define FOS_IRQ_ATA1      14
#define FOS_IRQ_ATA2      15

typedef void (*irq_handler_fn_t)(uint8_t irq);

void irq_init(void);
void irq_dispatch(uint8_t irq);

int  irq_register(uint8_t irq, irq_handler_fn_t handler);
int  irq_unregister(uint8_t irq);
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);
uint32_t irq_get_pending(void);
void irq_clear_pending(uint8_t irq);
int  irq_in_handler(void);
void irq_clear_handlers(void);
