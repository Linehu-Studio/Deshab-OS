/* deshab/tsc.h - TSC 时钟校准与延时
 *
 * 整合 dkm_shared.h 的 dkm_tsc_calibrate/delay_ms/delay_us/serial_wait_tx
 * 与 net_stack.h 的 ns_tsc_calibrate/rdtsc，统一前缀为 dsb_。
 *
 * 用法：
 *   dsb_tsc_calibrate()   - 在模块入口开头调用，PIT ch0 ~10ms 校准 TSC
 *   dsb_rdtsc()           - 读取 TSC 时间戳
 *   dsb_delay_ms/us()     - 基于 TSC 的忙等延时
 *   dsb_deadline_reached()- 截止时间判定
 *   dsb_serial_wait_tx()  - 串口发送就绪等待（100us 超时）
 *
 * 每个 .elf/.drv 包含本头后获得独立 file-local dsb_tsc_per_ms 副本。
 */
#ifndef DESHAB_TSC_H
#define DESHAB_TSC_H

#include "types.h"
#include "portio.h"

static u64 dsb_tsc_per_ms = 0;

static inline u64 dsb_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* PIT ch0 + TSC 校准 ~10ms 区间，推算每毫秒 TSC 周期数。 */
static inline void dsb_tsc_calibrate(void) {
    if (dsb_tsc_per_ms) return;
    dsb_outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    dsb_outb(0x40, 0x7C);       /* 11932 low = ~10ms */
    dsb_outb(0x40, 0x2E);       /* 11932 high */
    u64 tsc_start = dsb_rdtsc();
    u16 prev = 0;
    u64 loops = 0;
    for (;;) {
        dsb_outb(0x43, 0x00);
        u16 cur = (u16)dsb_inb(0x40) | ((u16)dsb_inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur;
        loops++;
    }
    dsb_tsc_per_ms = (dsb_rdtsc() - tsc_start) / 10;
    if (!dsb_tsc_per_ms) dsb_tsc_per_ms = 3000; /* 兜底 ~3GHz */
}

static inline int dsb_deadline_reached(u64 deadline) {
    return (i64)(dsb_rdtsc() - deadline) >= 0;
}

static inline void dsb_delay_ms(u32 ms) {
    if (!dsb_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000 * ms; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = dsb_tsc_per_ms * ms;
    u64 start = dsb_rdtsc();
    while (dsb_rdtsc() - start < target) __asm__ volatile("pause");
}

static inline void dsb_delay_us(u32 us) {
    if (!dsb_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100 * us; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = dsb_tsc_per_ms * us / 1000;
    u64 start = dsb_rdtsc();
    while (dsb_rdtsc() - start < target) __asm__ volatile("pause");
}

/* 串口发送就绪等待（100us 超时）。com_port = 0x3F8 (COM1)。 */
static inline void dsb_serial_wait_tx(u16 com_port) {
    if (!dsb_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000; i++) {
            if (dsb_inb((u16)(com_port + 5)) & 0x20) break;
            __asm__ volatile("pause");
        }
        return;
    }
    u64 deadline = dsb_rdtsc() + dsb_tsc_per_ms / 10;
    while (dsb_rdtsc() < deadline) {
        if (dsb_inb((u16)(com_port + 5)) & 0x20) break;
        __asm__ volatile("pause");
    }
}

#endif /* DESHAB_TSC_H */
