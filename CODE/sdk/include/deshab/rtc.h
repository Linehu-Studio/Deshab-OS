/* deshab/rtc.h - CMOS RTC 时钟读取
 *
 * 整合 desktop_app.h 的 da_rtc_time。读取 CMOS RTC 时/分（BCD -> 二进制）。
 */
#ifndef DESHAB_RTC_H
#define DESHAB_RTC_H

#include "types.h"
#include "portio.h"

static inline void dsb_rtc_time(u8 *h, u8 *m) {
    dsb_outb(0x70, 4);
    *h = dsb_inb(0x71);
    dsb_outb(0x70, 2);
    *m = dsb_inb(0x71);
    *h = (u8)((*h >> 4) * 10 + (*h & 0xF));
    *m = (u8)((*m >> 4) * 10 + (*m & 0xF));
}

#endif /* DESHAB_RTC_H */
