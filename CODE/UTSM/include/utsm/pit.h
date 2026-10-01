#ifndef UTSM_PIT_H
#define UTSM_PIT_H

#include <utsm/types.h>

/* PIT ch0 单发定时（LAPIC timer 校准用，见 arch/x86_64/lapic_timer.c）。
 * 模式 0（terminal count），单发不产生周期中断，不注册 IRQ handler。
 * 注意：不用 mode 3 —— B7 定论 PIT mode3 经 IOAPIC 边沿路由会双触发。 */

/* 编程 ch0 单发 ms 毫秒（>54ms 截断到 54ms） */
void pit_oneshot_ms(u32 ms);

/* latch 并返回 ch0 当前递减计数（16 位） */
u16 pit_latch_count(void);

/* 单发是否到期（两次 latch 回绕检测）。1=到期，0=未到期 */
int pit_oneshot_expired(void);

/* 静默：ch0 编程 mode0 计数 0，不再产生任何输出/中断 */
void pit_quiet(void);

#endif /* UTSM_PIT_H */
