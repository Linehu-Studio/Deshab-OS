#ifndef UTSM_IDT_H
#define UTSM_IDT_H

#include <utsm/types.h>

void idt_init(void);

/* 将 IDT 所有 256 个条目替换为安全 halt stub (cli; hlt)。
 * 用于 UTSM→DSK 跳转前，防止 DSK 运行期间过期 UTSM handler
 * 被 spurious IRQ / 异常触发导致三重故障重启。 */
void idt_halt_all(void);

/* IRQ handler: return 1 to suppress default ACK (PIC), 0 for normal EOI.
 * 参数为 IDT vector 号 (0x20-0xFF)。
 * PIC IRQ (0x20-0x2F): 返回 1 抑制 PIC EOI; 返回 0 自动发送 PIC EOI。
 * APIC/IOAPIC/MSI (0x30-0xFF): 返回值当前忽略, EOI 需手动处理 (TODO)。 */
typedef int (*irq_handler_t)(u8 vector);
int irq_register(u8 vector, irq_handler_t handler);

#endif
