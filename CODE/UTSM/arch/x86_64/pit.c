/* pit.c — PIT ch0 单发定时（LAPIC timer 校准用）
 *
 * 只用 mode 0（terminal count）：单发、无周期中断、不注册 IRQ handler。
 * 禁用 mode 3 —— B7 定论：PIT mode3 方波经 IOAPIC 边沿路由会双触发 (~200Hz)。
 */

#include <utsm/pit.h>

extern void outb(u16 port, u8 value);
extern u8 inb(u16 port);

#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43
#define PIT_BASE_HZ  1193182ULL

void pit_oneshot_ms(u32 ms) {
    u64 div = (PIT_BASE_HZ * (u64)ms) / 1000ULL;
    if (div == 0) div = 1;
    if (div > 65535) div = 65535;
    /* ch0 | lo/hi | mode0 (terminal count) | binary */
    outb(PIT_CMD, 0x30);
    outb(PIT_CH0_DATA, (u8)(div & 0xFF));
    outb(PIT_CH0_DATA, (u8)((div >> 8) & 0xFF));
}

u16 pit_latch_count(void) {
    outb(PIT_CMD, 0x00);   /* ch0 latch 计数（读前锁存） */
    u16 lo = inb(PIT_CH0_DATA);
    u16 hi = inb(PIT_CH0_DATA);
    return (u16)(lo | (hi << 8));
}

int pit_oneshot_expired(void) {
    u16 c1 = pit_latch_count();
    u16 c2 = pit_latch_count();
    /* mode0 计到 0 后回绕继续递减：cur 回升即视为到期 */
    return c2 >= c1;
}

void pit_quiet(void) {
    /* ch0 mode0、计数 0：terminal count 立即到期，无后续输出/中断 */
    outb(PIT_CMD, 0x30);
    outb(PIT_CH0_DATA, 0x00);
    outb(PIT_CH0_DATA, 0x00);
}
