/* host_main.h — Deshab 桌面 Linux 宿主接口
 *
 * 仅在 -DDESHAB_HOST 编译时使用。提供 SDL2 窗口（模拟 framebuffer）、
 * 鼠标/键盘事件（模拟 PS/2）、deshab.img 文件 block 后端、单调时钟
 * （替代 rDTSC）与当天秒数（替代 RTC CMOS）。
 *
 * desktop/main.c 通过 #ifdef DESHAB_HOST 调用本接口；裸机构建不包含本头。
 */
#ifndef DESHAB_HOST_MAIN_H
#define DESHAB_HOST_MAIN_H

#include <utsm/types.h>

/* dsk_entry 由 desktop/main.c 提供，host_main.c 构造好 ctx 后调用。 */
struct dsk_boot_context;
void dsk_entry(const struct dsk_boot_context *ctx);

/* ---- 日志（替代 COM1 串口 sputc/swrite/slog） ---- */
void host_log(const char *s);          /* 输出字符串到 stderr */
void host_log_char(char c);            /* 输出单字符到 stderr */

/* ---- 时间 ---- */
/* 单调时钟微秒，替代 rdtsc。供 tsc_calibrate/g_last_click_tsc 使用。
 * g_tsc_per_sec 应设为 1000000。 */
u64 host_now_us(void);
/* 当天秒数（0..86399），替代 rtc_day_sec。 */
int host_rtc_seconds(void);

/* ---- 鼠标（替代 ps2_mouse_poll） ----
 * 从 SDL 事件累积的鼠标状态生成一次快照。
 *   px/py        : 输出当前绝对坐标（已钳位到 [0,max]）
 *   pbtn         : 输出按键位（bit0=左 bit1=右）
 *   p_left_pressed / p_left_released / p_right_pressed : 边沿事件（1 表示本次发生）
 *   max_x/max_y  : 坐标上界（g_fb_w - CURSOR_SIZE 等）
 * 返回 1 表示有移动或按键变化，0 表示无事件。 */
int host_mouse_poll(int *px, int *py, int *pbtn,
                    int *p_left_pressed, int *p_left_released,
                    int *p_right_pressed, int max_x, int max_y);

/* ---- 键盘（替代 inb(0x64)/inb(0x60) 扫描码轮询） ----
 * 取一个 PS/2 set-1 扫描码（make 码；break 码为 |0x80）。
 * 返回 1 表示取到，0 表示队列空。 */
int host_kb_poll(u8 *out_sc);

/* ---- 帧末：pump SDL 事件 + present back buffer ----
 * 在主循环末尾调用（替代 __asm__("pause"))。
 *   1. 把 back buffer（g_host_backbuf）上传到 SDL texture 并 RenderPresent
 *   2. SDL_PollEvent 排空：鼠标/键盘事件入内部状态，SDL_QUIT 返回非 0
 * 返回非 0 表示收到退出请求（窗口关闭），调用方应置 g_quit。 */
int host_pump_and_present(void);

/* ---- host 分配的缓冲（替代裸机硬编码物理地址） ----
 * g_host_sprite : sprite 渲染缓冲（替代 SPRITE_BUF_ADDR 0x7000000）
 * g_host_backbuf: 真实 framebuffer 对应的 back buffer（替代 g_real_fb 指向的显存）
 * 大小均为 width*height*4 字节，由 host_main.c 在初始化时分配。 */
extern u8 *g_host_sprite;
extern u8 *g_host_backbuf;

#endif /* DESHAB_HOST_MAIN_H */
