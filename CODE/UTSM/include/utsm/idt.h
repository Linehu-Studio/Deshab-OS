#ifndef UTSM_IDT_H
#define UTSM_IDT_H

#include <utsm/types.h>

void idt_init(void);

/* 将 IDT 所有 256 个条目替换为安全 halt stub (cli; hlt)。
 * 用于 UTSM→DSK 跳转前，防止 DSK 运行期间过期 UTSM handler
 * 被 spurious IRQ / 异常触发导致三重故障重启。 */
void idt_halt_all(void);

/* DSK 诊断 IDT：vector 0-31 恢复原 isr_stub（异常走 idt_handler
 * 打完整日志后停机），vector 32-255 保持 halt stub。
 * BUG-20260801-005：UTSM→DSK 交接处以本函数替代 idt_halt_all，
 * 恢复 DSK 阶段 CPU 异常可见性（原静默 halt 零诊断）。
 * 不做 pic_remap / sti，DSK 上下文 IF=0 不变式保持不变。 */
void idt_install_dsk_diag(void);

/* PE 诊断 IDT：idt_install_dsk_diag 的别名（幂等重装）。
 * 用于 PE 兼容层原生执行 PE32+ 前恢复异常可见性。 */
void idt_install_pe_diag(void);

/* IRQ handler: return 1 to suppress default ACK (EOI), 0 for normal EOI.
 * 注册参数兼容两种命名空间（二者不重叠，自动判别）:
 *   - ISA IRQ 号 0-15    -> 自动换算为 vector 0x20+IRQ (兼容 ps2kbd/e1000/mouseInit)
 *   - IDT vector 0x20+   -> 直接按 vector 注册 (apic 0xE0 / virtio_net 0x20+n 现状)
 * vector 0-15 是 CPU 异常向量，idt_handler 从不作为 IRQ 分发目标，故无歧义。
 * EOI 语义:
 *   - PIC IRQ (0x20-0x2F): 返回 0 -> 先调 APIC EOI 钩子(若已注册)再发 8259 EOI;
 *     返回 1 -> 两者都抑制, handler 自负 EOI 全责。
 *   - APIC/IOAPIC/MSI (0x30-0xFF): 返回 0 -> 调 APIC EOI 钩子(若已注册);
 *     返回 1 -> 抑制(如 handler 内已自行 LAPIC EOI)。未注册钩子时不做任何 EOI。 */
typedef int (*irq_handler_t)(u8 vector);
int irq_register(u8 vector, irq_handler_t handler);

/* B7 阶段2: 注册 LAPIC EOI 钩子（由 apic.drv 在 LAPIC 接管后调用）。
 * 钩子语义: 向 LAPIC EOI 寄存器 (MMIO base+0xB0) 写 0，清除当前 in-service
 * 最高优先级 ISR 位。重复注册以最后一次为准；传 NULL 可注销。 */
void idt_register_apic_eoi(void (*eoi_fn)(void));

/* B7 阶段3: 动态 IDT 向量分配器 (MSI/MSI-X 用)。
 * 池范围 0x40-0xDF (160 个), 位图 O(1); 分配返回向量号, 池耗尽返回 -1。
 * free 幂等, 越界/静态向量输入忽略。主上下文调用, 无锁。 */
int  irq_vector_alloc(void);
void irq_vector_free(int vector);

/* 显式 EOI（Phase7: 调度器 tick 等可能切换任务的中断路径用）。
 * 在任何可能换栈的工作之前完成 EOI，防止 EOI 悬挂一个时间片。
 * 序列与 idt_handler 默认路径一致: LAPIC 钩子 → 8259 EOI（按向量范围）。
 * 参数兼容双命名空间（同 irq_register）: 0-15=ISA IRQ, 0x20+=向量。 */
void idt_irq_eoi(u8 vector);

/* CPU 异常钩子（Phase8: DRR 任务级 fault 接管）。
 * 在 idt_handler 的 vector<32 分支最前调用: 返回非 0 = 异常已被消费
 * （fault 任务被杀/切换，控制权不回到日志+停机路径）；返回 0 = 走原
 * 全量日志 + cli;hlt 路径（DSK 阶段行为不变）。传 NULL 注销。 */
typedef int (*idt_exception_hook_t)(u64 vector, u64 error_code, u64 rip);
void idt_set_exception_hook(idt_exception_hook_t hook);

#endif
