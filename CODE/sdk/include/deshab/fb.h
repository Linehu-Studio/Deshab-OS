/* deshab/fb.h - 帧缓冲绘制原语
 *
 * 整合 desktop_app.h 的 da_pixel/pixel_read/blend/fill_rect/fill_bg/
 * fill_rounded_rect/rect_outline，统一前缀为 dsb_。
 * 假设 32bpp BGRA 帧缓冲（Limine framebuffer 标准格式）。
 */
#ifndef DESHAB_FB_H
#define DESHAB_FB_H

#include "types.h"
#include "app.h"

static inline void dsb_pixel(dsb_app_context *ac, i64 x, i64 y, u32 color) {
    if (x < 0 || (u64)x >= ac->fb_w || y < 0 || (u64)y >= ac->fb_h) return;
    u32 *line = (u32 *)((u8 *)ac->fb + (u64)y * ac->fb_pitch);
    line[x] = color;
}

static inline u32 dsb_pixel_read(dsb_app_context *ac, i64 x, i64 y) {
    if (x < 0 || (u64)x >= ac->fb_w || y < 0 || (u64)y >= ac->fb_h) return 0;
    u32 *line = (u32 *)((u8 *)ac->fb + (u64)y * ac->fb_pitch);
    return line[x];
}

static inline u32 dsb_blend(u32 bg, u32 fg, u32 alpha) {
    u32 na = 256 - alpha;
    u32 r = ((bg & 0xFF) * na + (fg & 0xFF) * alpha) >> 8;
    u32 g = (((bg >> 8) & 0xFF) * na + ((fg >> 8) & 0xFF) * alpha) >> 8;
    u32 b = (((bg >> 16) & 0xFF) * na + ((fg >> 16) & 0xFF) * alpha) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline void dsb_fill_rect(dsb_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color) {
    for (i64 r = 0; r < h; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= ac->fb_h) continue;
        u32 *line = (u32 *)((u8 *)ac->fb + (u64)yy * ac->fb_pitch);
        for (i64 c = 0; c < w; c++) {
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= ac->fb_w) continue;
            line[xx] = color;
        }
    }
}

static inline void dsb_fill_bg(dsb_app_context *ac, u32 color) {
    dsb_fill_rect(ac, 0, 0, (i64)ac->fb_w, (i64)ac->fb_h, color);
}

static inline void dsb_fill_rounded_rect(dsb_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color, i64 radius) {
    i64 r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    dsb_fill_rect(ac, x + r, y, w - 2 * r, h, color);
    dsb_fill_rect(ac, x, y + r, r, h - 2 * r, color);
    dsb_fill_rect(ac, x + w - r, y + r, r, h - 2 * r, color);
    for (i64 dy = 0; dy < r; dy++)
        for (i64 dx = 0; dx < r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                dsb_pixel(ac, x + r - 1 - dx, y + r - 1 - dy, color);
                dsb_pixel(ac, x + w - r + dx, y + r - 1 - dy, color);
                dsb_pixel(ac, x + r - 1 - dx, y + h - r + dy, color);
                dsb_pixel(ac, x + w - r + dx, y + h - r + dy, color);
            }
}

static inline void dsb_rect_outline(dsb_app_context *ac, i64 x, i64 y, i64 w, i64 h, u32 color, i64 radius) {
    i64 r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (i64 c = r; c < w - r; c++) {
        dsb_pixel(ac, x + c, y, color);
        dsb_pixel(ac, x + c, y + h - 1, color);
    }
    for (i64 rr = r; rr < h - r; rr++) {
        dsb_pixel(ac, x, y + rr, color);
        dsb_pixel(ac, x + w - 1, y + rr, color);
    }
    for (i64 dy = 0; dy <= r; dy++)
        for (i64 dx = 0; dx <= r; dx++) {
            i64 d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - 1) * (r - 1)) {
                dsb_pixel(ac, x + r - dx, y + r - dy, color);
                dsb_pixel(ac, x + w - 1 - r + dx, y + r - dy, color);
                dsb_pixel(ac, x + r - dx, y + h - 1 - r + dy, color);
                dsb_pixel(ac, x + w - 1 - r + dx, y + h - 1 - r + dy, color);
            }
        }
}

#endif /* DESHAB_FB_H */
