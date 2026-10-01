#ifndef UTSM_LAPIC_TIMER_H
#define UTSM_LAPIC_TIMER_H

#include <utsm/types.h>

/* 宿主自持 LAPIC 定时器 —— SAS-R0-PCQ 调度器 tick 源（ROADMAP Phase 2 目标方案）。
 *
 * 动机：默认 FUCK 配置 apic_route=1 时，apic.drv 验证完 tick 链路后会把
 * GSI2（PIT IRQ0 线）重新 mask（apic.c route_switch 收尾），PIT 周期中断
 * 在该配置下不可达。LAPIC timer 不经 IOAPIC/PIC 路由，与路由模式解耦。
 *
 * 实现要点：
 *   - IA32_APIC_BASE MSR (0x1B) 取 LAPIC 物理基址，经 paging map_mmio 映射
 *     （SAS-R0：直接扩当前 CR3，全 CPU 可见）。
 *   - x2APIC（MSR bit10）不支持 → 返回负数，调度器降级为协作模式。
 *   - 频率校准：LAPIC 定时器 divide=16、initial=0xFFFFFFFF，用 PIT ch0
 *     单发 10ms 窗口采样 CCR 差值（pit.h 单发助手）。
 *   - 向量来自 irq_vector_alloc（0x40-0xDF 动态池）；EOI 由本模块直写
 *     LAPIC EOI 寄存器（不依赖 apic.drv 钩子，PIC/IOAPIC 两模式均成立）。
 */

/* 校准并配置周期定时器（LVT 保持 mask，尚未投递）。
 * 返回 0 成功，负数失败（x2APIC/映射失败/校准异常）。 */
int lapic_timer_init(u32 freq_hz);

/* init 成功后返回分配到的 IDT 向量（0x40-0xDF），未初始化返回 -1 */
int lapic_timer_vector(void);

/* LVT 解除 mask —— 开始周期投递（须在 irq_register 向量 handler 之后调用） */
void lapic_timer_start(void);

/* 停止：LVT 置 mask + initial count 清零 + 归还动态向量 */
void lapic_timer_stop(void);

/* 直写 LAPIC EOI 寄存器（tick handler 用；未初始化时为空操作） */
void lapic_timer_eoi(void);

#endif /* UTSM_LAPIC_TIMER_H */
