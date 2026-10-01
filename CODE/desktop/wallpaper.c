/* wallpaper.c — 壁纸背景层缓存实现（M2：磁盘加载版）
 *
 * 流程：FAT32 流式读 system/wallpaper.rgba（1000×700 RGBA 字节流）
 *   → WALLPAPER_STAGING_ADDR 暂存 → 双线性缩放（对齐像素中心，8.8 定点）
 *   → WALLPAPER_ADDR 背景层缓存。
 * 数组字节序 R,G,B,A；framebuffer u32 = 0xAARRGGBB（R 在 bit16）。
 */
#include "wallpaper.h"

/* 嵌入资产尺寸（与 gen_wallpaper.py 的 W×H 一致；运行时按文件大小校验） */
#define WP_SRC_W 1000
#define WP_SRC_H 700

static wp_u32 *g_wp_layer = 0;
static int g_wp_w = 0, g_wp_h = 0;
static wp_u32 g_wp_src_w = WP_SRC_W, g_wp_src_h = WP_SRC_H;

int wallpaper_init(wallpaper_read_fn read_to, int fb_w, int fb_h) {
    if (!read_to || fb_w <= 0 || fb_h <= 0) return -1;
    if ((wp_u64)fb_w * (wp_u64)fb_h * 4u > WALLPAPER_MAX) return -2;

    /* 1. 流式读原始 RGBA 到暂存区（PE surface 区，启动期独占） */
    wp_u32 src_total = g_wp_src_w * g_wp_src_h * 4u;
    if (src_total > WALLPAPER_STAGING_MAX) return -3;
    wp_u32 got = 0;
    if (read_to("system/wallpaper.rgba",
                (wp_u8 *)WALLPAPER_STAGING_ADDR, src_total, &got) != 0)
        return -4;
    if (got < src_total) {
        /* 文件比预期小：按实际行数截断（防御，正常部署不会发生） */
        if (got < g_wp_src_w * 4u) return -5;
        g_wp_src_h = got / (g_wp_src_w * 4u);
    }
    const wp_u8 *src = (const wp_u8 *)WALLPAPER_STAGING_ADDR;

    /* 2. 双线性缩放到背景层缓存 */
    g_wp_layer = (wp_u32 *)WALLPAPER_ADDR;
    g_wp_w = fb_w;
    g_wp_h = fb_h;
    int sw = (int)g_wp_src_w, sh = (int)g_wp_src_h;
    for (int y = 0; y < fb_h; y++) {
        wp_i64 fy64 = ((wp_i64)y * sh << 8) / fb_h;   /* 8.8 定点 */
        int sy0 = (int)(fy64 >> 8);
        int fy = (int)(fy64 & 0xFF);
        int sy1 = (sy0 + 1 < sh) ? sy0 + 1 : sy0;
        if (sy0 >= sh) sy0 = sy1 = sh - 1;
        for (int x = 0; x < fb_w; x++) {
            wp_i64 fx64 = ((wp_i64)x * sw << 8) / fb_w;
            int sx0 = (int)(fx64 >> 8);
            int fx = (int)(fx64 & 0xFF);
            int sx1 = (sx0 + 1 < sw) ? sx0 + 1 : sx0;
            if (sx0 >= sw) sx0 = sx1 = sw - 1;

            const wp_u8 *p00 = src + ((wp_i64)sy0 * sw + sx0) * 4;
            const wp_u8 *p10 = src + ((wp_i64)sy0 * sw + sx1) * 4;
            const wp_u8 *p01 = src + ((wp_i64)sy1 * sw + sx0) * 4;
            const wp_u8 *p11 = src + ((wp_i64)sy1 * sw + sx1) * 4;

            wp_u32 r, g, b;
            {
                wp_u32 rtop = ((wp_u32)p00[0] * (256 - fx) + (wp_u32)p10[0] * fx) >> 8;
                wp_u32 rbot = ((wp_u32)p01[0] * (256 - fx) + (wp_u32)p11[0] * fx) >> 8;
                wp_u32 gtop = ((wp_u32)p00[1] * (256 - fx) + (wp_u32)p10[1] * fx) >> 8;
                wp_u32 gbot = ((wp_u32)p01[1] * (256 - fx) + (wp_u32)p11[1] * fx) >> 8;
                wp_u32 btop = ((wp_u32)p00[2] * (256 - fx) + (wp_u32)p10[2] * fx) >> 8;
                wp_u32 bbot = ((wp_u32)p01[2] * (256 - fx) + (wp_u32)p11[2] * fx) >> 8;
                r = (rtop * (256 - fy) + rbot * fy) >> 8;
                g = (gtop * (256 - fy) + gbot * fy) >> 8;
                b = (btop * (256 - fy) + bbot * fy) >> 8;
            }
            g_wp_layer[(wp_i64)y * fb_w + x] =
                0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return 0;
}

wp_u32 *wallpaper_layer(void) {
    return g_wp_layer;
}
