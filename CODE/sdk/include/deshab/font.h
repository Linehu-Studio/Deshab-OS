/* deshab/font.h - ASCII 位图字体渲染
 *
 * 整合 desktop_app.h 的 da_draw_char/draw_string，统一前缀为 dsb_。
 *
 * 前置条件：包含者需先提供位图数据源。两种方式：
 *   1. #include "ascii_bitmaps.c"（定义 static g_ascii/g_ascii_w/g_ascii_h）
 *   2. 自行定义 const u8 *g_ascii[95]; i64 g_ascii_w, g_ascii_h;
 *   再 #include "font.h"。
 *
 * 与 deshab_ui.h 使用相同的包含顺序约定。
 */
#ifndef DESHAB_FONT_H
#define DESHAB_FONT_H

#include "types.h"
#include "app.h"
#include "fb.h"

/* 由包含者在 include 本头前提供：
 *   static const u8 *g_ascii[95];   // 95 字符 ' '..'~'
 *   static i64 g_ascii_w, g_ascii_h;
 */
static inline void dsb_draw_char(dsb_app_context *ac, char ch, i64 x, i64 y, u32 fg, u32 bg) {
    u32 idx = (u32)(ch - ' ');
    if (idx > 94) idx = 0;
    const u8 *glyph = g_ascii[idx];
    i64 gw = g_ascii_w, gh = g_ascii_h;
    for (i64 r = 0; r < gh; r++) {
        i64 yy = y + r;
        if (yy < 0 || (u64)yy >= ac->fb_h) continue;
        u32 *line = (u32 *)((u8 *)ac->fb + (u64)yy * ac->fb_pitch);
        for (i64 c = 0; c < gw; c++) {
            u8 a = glyph[r * gw + c];
            if (a == 0) continue;
            i64 xx = x + c;
            if (xx < 0 || (u64)xx >= ac->fb_w) continue;
            line[xx] = (a == 255) ? fg : dsb_blend(bg, fg, a);
        }
    }
}

static inline void dsb_draw_string(dsb_app_context *ac, const char *s, i64 x, i64 y, u32 fg, u32 bg, i64 step) {
    i64 cx = x;
    while (*s) {
        dsb_draw_char(ac, *s, cx, y, fg, bg);
        cx += step;
        s++;
    }
}

#endif /* DESHAB_FONT_H */
