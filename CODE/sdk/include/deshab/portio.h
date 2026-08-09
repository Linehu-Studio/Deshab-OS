/* deshab/portio.h - 端口 I/O 内联函数
 *
 * 统一 dkm_shared.h 的 dkm_outb/inb/outl/inl 与 desktop_app.h 的 outb/inb 两套实现。
 * 宿主已自带端口 I/O 声明时，包含前 #define DSB_NO_PORTIO 跳过本段。
 */
#ifndef DESHAB_PORTIO_H
#define DESHAB_PORTIO_H

#include "types.h"

#ifndef DSB_NO_PORTIO

static inline void dsb_outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline u8 dsb_inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void dsb_outw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline u16 dsb_inw(u16 port) {
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void dsb_outl(u16 port, u32 val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline u32 dsb_inl(u16 port) {
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#endif /* DSB_NO_PORTIO */

#endif /* DESHAB_PORTIO_H */
