/* deshab/serial.h - COM1 串口日志
 *
 * 整合 desktop_app.h 的 da_sputc/swrite/slog，统一前缀为 dsb_。
 * 用于无内核 log API 可用的早期阶段（应用启动、驱动 init 前）。
 */
#ifndef DESHAB_SERIAL_H
#define DESHAB_SERIAL_H

#include "types.h"
#include "portio.h"

#define DSB_COM1 0x3F8

static inline void dsb_sputc(char c) {
    for (unsigned i = 0; i < 100000; i++) {
        if (dsb_inb(DSB_COM1 + 5) & 0x20) break;
    }
    dsb_outb(DSB_COM1, (u8)c);
}

static inline void dsb_swrite(const char *s) {
    while (*s) {
        if (*s == '\n') dsb_sputc('\r');
        dsb_sputc(*s++);
    }
}

static inline void dsb_slog(const char *tag, const char *msg) {
    dsb_swrite("[");
    dsb_swrite(tag);
    dsb_swrite("] ");
    dsb_swrite(msg);
    dsb_swrite("\n");
}

#endif /* DESHAB_SERIAL_H */
