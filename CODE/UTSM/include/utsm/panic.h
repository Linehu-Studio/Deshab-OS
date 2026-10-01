#ifndef UTSM_PANIC_H
#define UTSM_PANIC_H

#include <utsm/types.h>

/* ===================================================================
 *  F1: 内核 panic 统一兜底（莲花崩溃屏）
 *
 *  崩了也得崩出莲花：
 *    - 帧缓冲直接绘制（不走 console_fb 驱动——崩溃时驱动可能已挂）
 *    - 淡金莲花 + panic_symbol + 消息 + 寄存器简表
 *    - DRR 归档（crash_buf）后 halt
 *
 *  本路径只依赖：Limine framebuffer（Limine 常驻映射）+ serial + DRR
 *  Emergency Pool（静态区），不依赖堆/驱动/中断。
 * =================================================================== */

/* 寄存器简表（可传 0：无异常帧的普通 panic） */
typedef struct panic_regs {
    u64 rip;
    u64 rsp;
    u64 rflags;
    u64 cs;
    u64 err;
    u64 vector;
    u64 cr2;
    u64 rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11;
} panic_regs;

/* 统一 panic 入口：画莲花崩溃屏 + 串口日志 + DRR 归档 + halt（不返回） */
void panic_full(const char *symbol, const char *msg, const panic_regs *regs);

/* 兼容旧接口：panic("msg") = panic_full("UTSM-PANIC", msg, 0) */
void panic(const char *msg);

#endif /* UTSM_PANIC_H */
