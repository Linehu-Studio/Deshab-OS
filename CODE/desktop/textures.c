/* textures.c — 裸 RGBA 纹理绘制实现（M2）
 *
 * 数组字节序 R,G,B,A；framebuffer u32 = 0xAARRGGBB。
 * 双线性插值（像素中心对齐，8.8 定点）+ src-over alpha。
 */
#include "ascii_font.h"   /* 顺序敏感：必须先于 textures.h（deshab_ui.h 需 g_ascii） */
#include "textures.h"

void tex_draw_scaled(du_context *ctx, const tex_u8 *rgba, int sw, int sh,
                     int x, int y, int w, int h, int dim) {
    if (!ctx || !rgba || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    if (dim < 0) dim = 0;
    if (dim > 256) dim = 256;

    int fw = (int)ctx->width, fh = (int)ctx->height;
    tex_u32 *fb = ctx->fb;
    tex_i64 pitch = (tex_i64)ctx->pitch / 4;   /* u32 单位 */

    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= fh) continue;
        tex_i64 fy64 = ((tex_i64)dy * sh << 8) / h;
        int sy0 = (int)(fy64 >> 8);
        int fy = (int)(fy64 & 0xFF);
        int sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sy0;
        if (sy0 >= sh) sy0 = sy1 = sh - 1;

        tex_u32 *dline = fb + (tex_i64)py * pitch;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= fw) continue;
            tex_i64 fx64 = ((tex_i64)dx * sw << 8) / w;
            int sx0 = (int)(fx64 >> 8);
            int fx = (int)(fx64 & 0xFF);
            int sx1 = (sx0 + 1 < sw) ? sx0 + 1 : sx0;
            if (sx0 >= sw) sx0 = sx1 = sw - 1;

            const tex_u8 *p00 = rgba + ((tex_i64)sy0 * sw + sx0) * 4;
            const tex_u8 *p10 = rgba + ((tex_i64)sy0 * sw + sx1) * 4;
            const tex_u8 *p01 = rgba + ((tex_i64)sy1 * sw + sx0) * 4;
            const tex_u8 *p11 = rgba + ((tex_i64)sy1 * sw + sx1) * 4;

            tex_u32 a, r, g, b;
            {
                tex_u32 rtop = ((tex_u32)p00[0] * (256 - fx) + (tex_u32)p10[0] * fx) >> 8;
                tex_u32 rbot = ((tex_u32)p01[0] * (256 - fx) + (tex_u32)p11[0] * fx) >> 8;
                tex_u32 gtop = ((tex_u32)p00[1] * (256 - fx) + (tex_u32)p10[1] * fx) >> 8;
                tex_u32 gbot = ((tex_u32)p01[1] * (256 - fx) + (tex_u32)p11[1] * fx) >> 8;
                tex_u32 btop = ((tex_u32)p00[2] * (256 - fx) + (tex_u32)p10[2] * fx) >> 8;
                tex_u32 bbot = ((tex_u32)p01[2] * (256 - fx) + (tex_u32)p11[2] * fx) >> 8;
                tex_u32 atop = ((tex_u32)p00[3] * (256 - fx) + (tex_u32)p10[3] * fx) >> 8;
                tex_u32 abot = ((tex_u32)p01[3] * (256 - fx) + (tex_u32)p11[3] * fx) >> 8;
                r = (rtop * (256 - fy) + rbot * fy) >> 8;
                g = (gtop * (256 - fy) + gbot * fy) >> 8;
                b = (btop * (256 - fy) + bbot * fy) >> 8;
                a = (atop * (256 - fy) + abot * fy) >> 8;
            }
            if (a == 0) continue;

            /* 亮度衰减（boot 背景） */
            r = r * dim >> 8; g = g * dim >> 8; b = b * dim >> 8;

            tex_u32 dst = dline[px];
            if (a >= 255) {
                dline[px] = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else {
                tex_u32 dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
                tex_u32 na = 255 - a;
                dline[px] = 0xFF000000u |
                    (((r * a + dr * na) >> 8) << 16) |
                    (((g * a + dg * na) >> 8) << 8) |
                    ((b * a + db * na) >> 8);
            }
        }
    }
}
