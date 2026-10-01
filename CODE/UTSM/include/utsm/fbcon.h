#ifndef UTSM_FBCON_H
#define UTSM_FBCON_H

#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

/* boot_modules.c 提供（Limine framebuffer 请求） */
extern volatile struct limine_framebuffer_request g_fb_request;

/* ===================================================================
 *  F2: 统一帧缓冲控制台抽象（fbcon）
 *
 *  让"疯要有画面"不绑定任何一个驱动：
 *    - 崩溃路径（panic.c）：驱动可能已挂，直接用 Limine fb
 *    - 早期控制台 / 任何需要直绘的 UTSM 内部路径
 *
 *  32bpp XRGB，全部带边界检查，无堆依赖。
 *  文本渲染需要先 include 字库（g_ascii/g_ascii_w/g_ascii_h，
 *  见 kernel/panic_font.inc）。
 * =================================================================== */

typedef struct {
    u32 *fb;
    u64  w, h, pitch;
} fbc_ctx;

/* 从 Limine framebuffer 初始化；失败 -1（无 fb / 非 32bpp） */
static inline int fbc_init(fbc_ctx *c) {
    if (!g_fb_request.response || g_fb_request.response->framebuffer_count == 0)
        return -1;
    struct limine_framebuffer *fb = g_fb_request.response->framebuffers[0];
    if (fb->bpp < 32 || !fb->address) return -1;
    c->fb = (u32 *)fb->address;
    c->w = fb->width;
    c->h = fb->height;
    c->pitch = fb->pitch;
    return 0;
}

static inline void fbc_pixel(fbc_ctx *c, i64 x, i64 y, u32 col) {
    if (x < 0 || (u64)x >= c->w || y < 0 || (u64)y >= c->h) return;
    *(u32 *)((u8 *)c->fb + (u64)y * c->pitch + (u64)x * 4) = col;
}

static inline u32 fbc_pixel_read(fbc_ctx *c, i64 x, i64 y) {
    if (x < 0 || (u64)x >= c->w || y < 0 || (u64)y >= c->h) return 0;
    return *(u32 *)((u8 *)c->fb + (u64)y * c->pitch + (u64)x * 4);
}

/* alpha 混合（0–255） */
static inline u32 fbc_blend(u32 bg, u32 fg, u32 alpha) {
    u32 na = 256 - alpha;
    u32 r = (((bg >> 16) & 0xFF) * na + ((fg >> 16) & 0xFF) * alpha) >> 8;
    u32 g = (((bg >> 8) & 0xFF) * na + ((fg >> 8) & 0xFF) * alpha) >> 8;
    u32 b = ((bg & 0xFF) * na + (fg & 0xFF) * alpha) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline void fbc_fill_rect(fbc_ctx *c, i64 x, i64 y, i64 w, i64 h, u32 col) {
    for (i64 r = 0; r < h; r++)
        for (i64 cx = 0; cx < w; cx++)
            fbc_pixel(c, x + cx, y + r, col);
}

/* 全屏纯色（背景色填充） */
static inline void fbc_fill_bg(fbc_ctx *c, u32 col) {
    for (u64 y = 0; y < c->h; y++) {
        u32 *line = (u32 *)((u8 *)c->fb + y * c->pitch);
        for (u64 x = 0; x < c->w; x++) line[x] = col;
    }
}

/* RGBA8888 图像绘制（半透明像素与已绘背景混合） */
static inline void fbc_draw_rgba(fbc_ctx *c, const unsigned char *d,
                                 i64 w, i64 h, i64 x, i64 y) {
    for (i64 r = 0; r < h; r++) {
        for (i64 cx = 0; cx < w; cx++) {
            const unsigned char *px4 = d + ((u64)r * w + (u64)cx) * 4;
            u32 a = px4[3];
            if (a < 8) continue;
            u32 col = 0xFF000000u | ((u32)px4[0] << 16) | ((u32)px4[1] << 8) | px4[2];
            if (a < 250) col = fbc_blend(fbc_pixel_read(c, x + cx, y + r), col, a);
            fbc_pixel(c, x + cx, y + r, col);
        }
    }
}

/* ---- 文本 ----
 * 字库（g_ascii / g_ascii_w / g_ascii_h）由使用方先 include（如
 * kernel/panic_font.inc），并定义 FBCON_HAVE_FONT 后再包含本头文件。 */
#ifdef FBCON_HAVE_FONT

static inline void fbc_char(fbc_ctx *c, char ch, i64 x, i64 y, u32 fg) {
    u32 idx = (u32)ch - 32;
    if (idx > 94) idx = 0;
    const u8 *g = g_ascii[idx];
    for (i64 r = 0; r < g_ascii_h; r++)
        for (i64 cc = 0; cc < g_ascii_w; cc++) {
            u8 a = g[r * g_ascii_w + cc];
            if (a < 24) continue;
            fbc_pixel(c, x + cc, y + r, fg);
        }
}

static inline void fbc_string(fbc_ctx *c, const char *s, i64 x, i64 y, u32 fg) {
    i64 cx = x;
    while (*s) {
        fbc_char(c, *s, cx, y, fg);
        cx += g_ascii_w;
        s++;
    }
}

static inline void fbc_string_center(fbc_ctx *c, const char *s, i64 y, u32 fg) {
    i64 w = 0;
    for (const char *q = s; *q; q++) w += g_ascii_w;
    fbc_string(c, s, ((i64)c->w - w) / 2, y, fg);
}

#endif /* FBCON_HAVE_FONT */

#endif /* UTSM_FBCON_H */
