#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/drr.h>

/* ===================================================================
 *  F1: 莲花崩溃屏（panic_full）
 *
 *  "崩了也得崩出莲花。"
 *  帧缓冲直绘（fbcon 抽象，F2）：DEAICUP 莲花 Logo（ohMyLogo.png
 *  内嵌）+ panic_symbol + 消息 + 寄存器简表，背景色与图片背景一致，
 *  DRR crash_buf 归档后 halt。不依赖驱动/堆/中断。
 * =================================================================== */

void arch_halt_forever(void);

/* 11x18 Consolas 灰度字库（同 firstInit/ascii_bitmaps.c，自动生成）。
 * 先包含字库再定义 FBCON_HAVE_FONT 引出 fbcon 文本接口。 */
#include "panic_font.inc"
#define FBCON_HAVE_FONT 1

#include <utsm/fbcon.h>

/* DEAICUP 莲花 Logo（ohMyLogo.png 缩放 384px，RGBA8888）+
 * 背景色（图片四角平均色）。生成：kernel/gen_panic_logo.py */
#include "panic_logo_data.inc"

/* ---- hex 输出 ---- */

static void phex64(fbc_ctx *p, u64 v, i64 x, i64 y, u32 fg) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (i32 i = 0; i < 16; i++) {
        u32 nib = (u32)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    buf[18] = 0;
    fbc_string(p, buf, x, y, fg);
}

static void pregline(fbc_ctx *p, const char *name, u64 v, i64 x, i64 y, u32 fg) {
    fbc_string(p, name, x, y, fg);
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
    fbc_ctx P, *p = &P;
    if (fbc_init(p) == 0) {
        /* 背景色 = Logo 图片背景色（四角平均，gen_panic_logo.py 生成） */
        fbc_fill_bg(p, g_panic_logo_bg);

        /* DEAICUP 莲花 Logo 居中，文字随 logo 下缘排布 */
        i64 ly = ((i64)p->h - (i64)g_panic_logo_h) / 2 - 40;
        if (ly < 8) ly = 8;
        fbc_draw_rgba(p, g_panic_logo_rgba,
                      (i64)g_panic_logo_w, (i64)g_panic_logo_h,
                      ((i64)p->w - (i64)g_panic_logo_w) / 2, ly);

        const u32 fg = 0xFFF0E8D8, dim = 0xFFB0A8C0, acc = 0xFFF0D080;
        i64 y = ly + (i64)g_panic_logo_h + 8;
        fbc_string_center(p, "DESHAB PANIC", y, acc);
        y += g_ascii_h + 8;
        if (symbol) { fbc_string_center(p, symbol, y, fg); y += g_ascii_h + 8; }
        if (msg) {
            /* 单行截断 60 字符 */
            char line[61];
            u32 i = 0;
            while (msg[i] && i < 60) { line[i] = msg[i]; i++; }
            line[i] = 0;
            fbc_string_center(p, line, y, fg);
            y += g_ascii_h + 8;
        }
        if (regs) {
            if (y + g_ascii_h * 6 < (i64)p->h) {
                pregline(p, "RIP ", regs->rip, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                pregline(p, "RSP ", regs->rsp, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                pregline(p, "FLG ", regs->rflags, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim); y += g_ascii_h + 2;
                if (regs->vector == 14) {
                    pregline(p, "CR2 ", regs->cr2, (i64)p->w / 2 - 14 * g_ascii_w / 2, y, dim);
                }
            }
        }
        fbc_string_center(p, archived == 0 ? "crash archived to DRR" : "DRR archive unavailable",
                          (i64)p->h - g_ascii_h - 24, dim);
    }

    log_error("[PANIC] halt");
    arch_halt_forever();
}

void panic(const char *msg) {
    panic_full("UTSM-PANIC", msg, 0);
}
