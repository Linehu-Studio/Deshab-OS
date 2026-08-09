/* deshab/input.h - PS/2 键盘与鼠标输入
 *
 * 整合 desktop_app.h 的 da_scan_to_ascii / da_mouse / da_mouse_init /
 * da_mouse_poll / da_ps2_drain，统一前缀为 dsb_。
 *
 * PS/2 鼠标初始化序列与 desktop/main.c:ps2_mouse_init 保持一致，
 * 必须先冲刷输出缓冲再发命令，避免残留字节污染配置字
 * （BUG-20260801-008）。
 */
#ifndef DESHAB_INPUT_H
#define DESHAB_INPUT_H

#include "types.h"
#include "portio.h"
#include "cursor.h"

/* ---- 键盘 scan code set 1 -> ASCII ---- */
static inline char dsb_scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* ---- PS/2 鼠标 ---- */
typedef struct dsb_mouse {
    u8 buf[3];
    int idx;
    int has_pkt;
} dsb_mouse;

/* 冲刷 PS/2 控制器输出缓冲（最多 16 字节）。 */
static inline void dsb_ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(dsb_inb(0x64) & 1)) break;
        dsb_inb(0x60);
    }
}

/* 初始化 PS/2 鼠标：启用 AUX 端口 + 流模式 + 数据报告。
 * 序列与 desktop/main.c:ps2_mouse_init 一致。 */
static inline void dsb_mouse_init(void) {
    dsb_ps2_drain();
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x64, 0xA8);
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x64, 0x20);
    for (int t = 0; t < 100000; t++) { if (dsb_inb(0x64) & 1) break; }
    u8 cfg = dsb_inb(0x60);
    cfg &= ~0x20; cfg |= 0x02; cfg |= 0x40; /* AUX 时钟启用 + AUX IRQ + 键盘翻译 */
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x64, 0x60);
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x60, cfg);
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x64, 0xD4);
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x60, 0xFF);
    for (int i = 0; i < 8; i++) {           /* 读完复位全部响应(FA AA 00) */
        int got = 0;
        for (int t = 0; t < 100000; t++) { if (dsb_inb(0x64) & 1) { got = 1; break; } }
        if (!got) break;
        dsb_inb(0x60);
    }
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x64, 0xD4);
    for (int t = 0; t < 100000; t++) { if (!(dsb_inb(0x64) & 2)) break; }
    dsb_outb(0x60, 0xF4);
    for (int i = 0; i < 4; i++) {           /* 读完使能 ACK */
        int got = 0;
        for (int t = 0; t < 100000; t++) { if (dsb_inb(0x64) & 1) { got = 1; break; } }
        if (!got) break;
        dsb_inb(0x60);
    }
    dsb_ps2_drain();
}

/* 轮询鼠标：3 字节包解码，更新光标坐标与按键状态。
 * 返回 1=收到完整包，0=无数据/未完成。
 * 必须先查 AUX 位(bit5)再读数据端口，避免吞掉键盘扫描码
 * （BUG-20260801-003）。 */
static inline int dsb_mouse_poll(dsb_mouse *m, dsb_cursor *c, i64 fb_w, i64 fb_h) {
    u8 st = dsb_inb(0x64);
    if (!(st & 1)) return 0;
    if (!(st & 0x20)) return 0;  /* 非 AUX 数据，留给键盘路径 */
    u8 data = dsb_inb(0x60);
    m->buf[m->idx++] = data;
    if (m->idx < 3) return 0;
    m->idx = 0;
    if (!(m->buf[0] & 0x08)) return 0;  /* 同步位验证 */
    int dx = (int)(signed char)m->buf[1];
    int dy = (int)(signed char)m->buf[2];
    dy = -dy;  /* PS/2 Y 向上为正，屏幕 Y 向下为正 */
    c->mx += dx;
    c->my += dy;
    if (c->mx < 0) c->mx = 0;
    if (c->my < 0) c->my = 0;
    if (c->mx >= (int)fb_w - DSB_CURSOR_SIZE) c->mx = (int)fb_w - DSB_CURSOR_SIZE;
    if (c->my >= (int)fb_h - DSB_CURSOR_SIZE) c->my = (int)fb_h - DSB_CURSOR_SIZE;
    c->btn = (m->buf[0] & 0x01) ? 1 : 0;
    m->has_pkt = 1;
    return 1;
}

#endif /* DESHAB_INPUT_H */
