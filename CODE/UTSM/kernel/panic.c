#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/drr.h>
#include "../arch/x86_64/limine.h"

/* ===================================================================
 *  F1: 莲花崩溃屏（panic_full）
 *
 *  "崩了也得崩出莲花。"
 *  帧缓冲直绘：淡金莲花 + panic_symbol + 消息 + 寄存器简表，
 *  DRR crash_buf 归档后 halt。不依赖驱动/堆/中断。
 * =================================================================== */

void arch_halt_forever(void);

extern volatile struct limine_framebuffer_request g_fb_request;

/* 11x18 Consolas 灰度字库（同 firstInit/ascii_bitmaps.c，自动生成） */
#include "panic_font.inc"

/* ---- 最小绘制原语（32bpp XRGB，带边界检查） ---- */

typedef struct {
    u32 *fb;
    u64  w, h, pitch;
} pfb;

static int pfb_init(pfb *p) {
    if (!g_fb_request.response || g_fb_request.response->framebuffer_count == 0)
        return -1;
    struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
    if (fb->bpp < 32 || !fb->address) return -1;
    p->fb = (u32 *)fb->address;
    p->w = fb->width;
    p->h = fb->height;
    p->pitch = fb->pitch;
    return 0;
}

static inline void ppx(pfb *p, i64 x, i64 y, u32 c) {
    if (x < 0 || (u64)x >= p->w || y < 0 || (u64)y >= p->h) return;
    *(u32 *)((u8 *)p->fb + (u64)y * p->pitch + (u64)x * 4) = c;
}

static void prect(pfb *p, i64 x, i64 y, i64 w, i64 h, u32 c) {
    for (i64 r = 0; r < h; r++)
        for (i64 c2 = 0; c2 < w; c2++)
            ppx(p, x + c2, y + r, c);
}

static u32 pgrad(u32 top, u32 bot, i64 y, i64 h) {
    if (h <= 1) return top;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    u32 a = (u32)(((u64)y * 255ULL) / (u64)(h - 1));
    u32 na = 256 - a;
    u32 r = (((top >> 16) & 0xFF) * na + ((bot >> 16) & 0xFF) * a) >> 8;
    u32 g = (((top >> 8) & 0xFF) * na + ((bot >> 8) & 0xFF) * a) >> 8;
    u32 b = ((top & 0xFF) * na + (bot & 0xFF) * a) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* 11x18 灰度字符（g_ascii: ASCII 32..126） */
static void pchar(pfb *p, char ch, i64 x, i64 y, u32 fg) {
    u32 idx = (u32)ch - 32;
    if (idx > 94) idx = 0;
    const u8 *g = g_ascii[idx];
    for (i64 r = 0; r < g_ascii_h; r++) {
        for (i64 c = 0; c < g_ascii_w; c++) {
            u8 a = g[r * g_ascii_w + c];
            if (a < 24) continue;
            ppx(p, x + c, y + r, fg);
        }
    }
}

static void pstr(pfb *p, const char *s, i64 x, i64 y, u32 fg) {
    i64 cx = x;
    while (*s) {
        pchar(p, *s, cx, y, fg);
        cx += g_ascii_w;
        s++;
    }
}

static void pstr_center(pfb *p, const char *s, i64 y, u32 fg) {
    i64 w = 0;
    for (const char *q = s; *q; q++) w += g_ascii_w;
    pstr(p, s, ((i64)p->w - w) / 2, y, fg);
}

/* ---- 莲花：8 瓣椭圆花瓣 + 金芯（淡金 on 暗底） ---- */

/* sin 表：33 项覆盖 [0, π/2]，步长 (π/2)/32，值×1000。
 * cos(i) = SIN33[32-i]。视觉用途精度足够。 */
static const i16 SIN33[33] = {
    0,   49,  98,  146, 195, 242, 290, 336,
    383, 428, 471, 513, 556, 597, 635, 671,
    707, 740, 773, 803, 831, 857, 882, 905,
    924, 941, 958, 972, 984, 993, 999, 1000, 1000
};

/* a: q12 角度（0..3071，3072=整圈），输出 sin/cos ×1000 */
static void sincos_q12(i64 a, i64 *so, i64 *co) {
    a %= 3072;
    if (a < 0) a += 3072;
    i32 qs = (i32)(a / 768);
    i32 idx = (i32)((a % 768) / 24);
    i64 s = 0, c = 0;
    switch (qs) {
        case 0: s = SIN33[idx];        c = SIN33[32 - idx];  break;
        case 1: s = SIN33[32 - idx];   c = -SIN33[idx];      break;
        case 2: s = -SIN33[idx];       c = -SIN33[32 - idx]; break;
        default: s = -SIN33[32 - idx]; c = SIN33[idx];       break;
    }
    *so = s;
    *co = c;
}

static void petal(pfb *p, i64 cx, i64 cy, i64 angle_q12, i64 len, i64 wid, u32 color) {
    /* 花瓣局部坐标系：u 沿长轴（尖端收窄），v 沿短轴；旋转到屏幕坐标 */
    i64 s, c;
    sincos_q12(angle_q12, &s, &c);
    i64 len1000 = len ? len * 1000 : 1;
    i64 wid1000 = wid ? wid * 1000 : 1;
    for (i64 u = -len; u <= len; u++) {
        for (i64 v = -wid; v <= wid; v++) {
            i64 eu = (u * 1000 * 1000) / len1000;   /* -1000..1000 */
            i64 ev = (v * 1000 * 1000) / wid1000;
            if (eu > 200) ev = (ev * (1000 - (eu - 200) * 2)) / 1000; /* 收尖 */
            if (eu * eu + ev * ev > 1000 * 1000) continue;
            i64 dx = (u * c - v * s) / 1000;
            i64 dy = (u * s + v * c) / 1000;
            ppx(p, cx + dx, cy + dy, color);
        }
    }
}

static void lotus(pfb *p, i64 cx, i64 cy) {
    const u32 petal_c  = 0xFFD8B4E8;  /* 淡粉金 */
    const u32 petal_c2 = 0xFFC498D8;  /* 深一档 */
    const u32 core_c   = 0xFFF0D080;  /* 金芯 */
    for (i64 k = 0; k < 8; k++) {
        i64 ang = k * 384;            /* 3072/8 */
        petal(p, cx, cy, ang, 46, 15, (k & 1) ? petal_c : petal_c2);
    }
    /* 金芯圆 */
    for (i64 dy = -12; dy <= 12; dy++)
        for (i64 dx = -12; dx <= 12; dx++)
            if (dx * dx + dy * dy <= 144)
                ppx(p, cx + dx, cy + dy, core_c);
    /* 芯内小点 */
    for (i64 dy = -4; dy <= 4; dy++)
        for (i64 dx = -4; dx <= 4; dx++)
            if (dx * dx + dy * dy <= 16)
                ppx(p, cx + dx, cy + dy, 0xFF7050A0);
}

/* ---- hex 输出 ---- */

static void phex64(pfb *p, u64 v, i64 x, i64 y, u32 fg) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (i32 i = 0; i < 16; i++) {
        u32 nib = (u32)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    buf[18] = 0;
    pstr(p, buf, x, y, fg);
}

static void pregline(pfb *p, const char *name, u64 v, i64 x, i64 y, u32 fg) {
    pstr(p, name, x, y, fg);
    phex64(p, v, x + 5 * g_ascii_w, y, fg);
}

/* ---- DRR 归档（drr.h 声明，drr_stub.c 提供；未初始化时返回 -1） ---- */

void panic_full(const char *symbol, const char *msg, const panic_regs *regs) {
    log_error("[PANIC] ==========================================");
    log_error("[PANIC] panic_full: 莲花崩溃屏");
    if (symbol) { log_error("[PANIC] symbol:"); log_error(symbol); }
    if (msg)    { log_error("[PANIC] msg:");    log_error(msg); }
    if (regs) {
        log_hex64("[PANIC] vector=", regs->vector);
        log_hex64("[PANIC] err=", regs->err);
        log_hex64("[PANIC] rip=", regs->rip);
        log_hex64("[PANIC] rsp=", regs->rsp);
        log_hex64("[PANIC] rflags=", regs->rflags);
        log_hex64("[PANIC] cr2=", regs->cr2);
        log_hex64("[PANIC] rax=", regs->rax);
        log_hex64("[PANIC] rcx=", regs->rcx);
        log_hex64("[PANIC] rdx=", regs->rdx);
    }

    int archived = drr_crash_archive(symbol, msg, regs);
    if (archived == 0) {
        log_error("[PANIC] 本次崩溃已归档至 DRR");
    } else {
        log_warn("[PANIC] DRR 归档不可用（未初始化），仅留串口日志");
    }

    /* 屏幕绘制放在归档之后：绘制循环中再出异常也不丢归档 */
    pfb P, *p = &P;
    if (pfb_init(p) == 0) {
        /* 暗夜渐变：深蓝 → 暗紫 */
        const u32 top = 0xFF101830, bot = 0xFF241430;
        for (u64 y = 0; y < p->h; y++) {
            u32 c = pgrad(top, bot, (i64)y, (i64)p->h);
            u32 *line = (u32 *)((u8 *)p->fb + y * p->pitch);
            for (u64 x = 0; x < p->w; x++) line[x] = c;
        }

        lotus(p, (i64)p->w / 2, (i64)p->h / 2 - 120);

        const u32 fg = 0xFFF0E8D8, dim = 0xFFB0A8C0, acc = 0xFFF0D080;
        pstr_center(p, "DESHAB PANIC", (i64)p->h / 2 + 10, acc);

        i64 y = (i64)p->h / 2 + 60;
        if (symbol) pstr_center(p, symbol, y, fg), y += g_ascii_h + 8;
        if (msg) {
            /* 单行截断 60 字符 */
            char line[61];
            u32 i = 0;
            while (msg[i] && i < 60) { line[i] = msg[i]; i++; }
            line[i] = 0;
            pstr_center(p, line, y, fg), y += g_ascii_h + 8;
        }
        if (regs) {
            if (y + g_ascii_h * 6 < (i64)p->h) {
                pregline(p, "RIP ", regs->rip, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                pregline(p, "RSP ", regs->rsp, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                pregline(p, "FLG ", regs->rflags, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                if (regs->vector == 14) {
                    pregline(p, "CR2 ", regs->cr2, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                }
            }
        }
        pstr_center(p, archived == 0 ? "crash archived to DRR" : "DRR archive unavailable",
                    (i64)p->h - g_ascii_h - 24, dim);
    }

    log_error("[PANIC] halt");
    arch_halt_forever();
}

void panic(const char *msg) {
    panic_full("UTSM-PANIC", msg, 0);
}
