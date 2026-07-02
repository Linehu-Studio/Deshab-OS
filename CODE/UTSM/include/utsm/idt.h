#ifndef UTSM_IDT_H
#define UTSM_IDT_H

#include <utsm/types.h>

void idt_init(void);

/* IRQ handler: return 1 to suppress default ACK, 0 for normal EOI */
typedef int (*irq_handler_t)(u8 irq);
int irq_register(u8 irq, irq_handler_t handler);

#endif
