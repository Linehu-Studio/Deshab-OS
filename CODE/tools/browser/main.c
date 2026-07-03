/* Deshab Browser — 全屏浏览器 (Stub)
 * 地址栏 + 导航按钮 + 内容渲染区域。
 * 当前显示 stub 提示，Esc 退出。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"

#define CHAR_STEP  12
#define CHAR_H     18

static da_app_context g_ac;
static da_cursor g_cursor;
static da_mouse g_mouse;

static void redraw_all(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);

    /* 标题栏 */
    da_draw_titlebar(&g_ac, "Browser", (i64)g_ac.fb_w);

    /* 地址栏 */
    int bar_y = DA_TITLEBAR_H + 6;
    int bar_h = 28;
    int bar_x = 12;
    int bar_w = (int)g_ac.fb_w - 24;
    da_fill_rounded_rect(&g_ac, bar_x, bar_y, bar_w, bar_h, DA_BG_TERTIARY, 6);
    da_rect_outline(&g_ac, bar_x, bar_y, bar_w, bar_h, DA_BORDER, 6);

    /* 导航按钮 */
    int btn_w = 32;
    da_fill_rounded_rect(&g_ac, bar_x + 4, bar_y + 4, btn_w, bar_h - 8, DA_BG_SECONDARY, 4);
    da_draw_string(&g_ac, "<", bar_x + 14, bar_y + 5, DA_TEXT_PRIMARY, DA_BG_SECONDARY, CHAR_STEP);
    da_fill_rounded_rect(&g_ac, bar_x + 4 + btn_w + 4, bar_y + 4, btn_w, bar_h - 8, DA_BG_SECONDARY, 4);
    da_draw_string(&g_ac, ">", bar_x + 4 + btn_w + 4 + 12, bar_y + 5, DA_TEXT_PRIMARY, DA_BG_SECONDARY, CHAR_STEP);
    da_fill_rounded_rect(&g_ac, bar_x + 4 + (btn_w + 4) * 2, bar_y + 4, btn_w, bar_h - 8, DA_BG_SECONDARY, 4);
    da_draw_string(&g_ac, "R", bar_x + 4 + (btn_w + 4) * 2 + 12, bar_y + 5, DA_TEXT_PRIMARY, DA_BG_SECONDARY, CHAR_STEP);

    /* URL 文本 */
    int url_x = bar_x + (btn_w + 4) * 3 + 8;
    da_draw_string(&g_ac, "deshab://home", url_x, bar_y + 5, DA_TEXT_DIM, DA_BG_TERTIARY, CHAR_STEP);

    /* 内容区域 */
    int content_y = bar_y + bar_h + 12;
    int content_x = 16;
    int content_w = (int)g_ac.fb_w - 32;

    /* Stub 页面 */
    da_fill_rounded_rect(&g_ac, content_x, content_y, content_w, 200, DA_BG_SECONDARY, 8);
    da_rect_outline(&g_ac, content_x, content_y, content_w, 200, DA_BORDER, 8);

    /* 居中图标 */
    int icon_cx = content_x + content_w / 2 - 32;
    int icon_cy = content_y + 24;
    da_fill_rounded_rect(&g_ac, icon_cx, icon_cy, 64, 48, DA_BG_TERTIARY, 8);
    /* 地球图标简化：圆圈 */
    int cx = icon_cx + 32, cy = icon_cy + 24, r = 16;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx+dy*dy <= r*r && dx*dx+dy*dy > (r-1)*(r-1))
                da_pixel(&g_ac, cx+dx, cy+dy, DA_ACCENT);
    /* 横线 */
    for (int dx = -r; dx <= r; dx++)
        if (dx*dx + 0 <= r*r)
            da_pixel(&g_ac, cx+dx, cy, DA_ACCENT);
    /* 竖线 */
    for (int dy = -r; dy <= r; dy++)
        if (0 + dy*dy <= r*r)
            da_pixel(&g_ac, cx, cy+dy, DA_ACCENT);

    /* 提示文字 */
    da_draw_string(&g_ac, "Deshab Browser",
                   content_x + content_w / 2 - 7 * CHAR_STEP, icon_cy + 60,
                   DA_TEXT_PRIMARY, DA_BG_SECONDARY, CHAR_STEP);
    da_draw_string(&g_ac, "HTTP + HTML rendering required",
                   content_x + content_w / 2 - 15 * CHAR_STEP, icon_cy + 82,
                   DA_TEXT_DIM, DA_BG_SECONDARY, CHAR_STEP);
    da_draw_string(&g_ac, "Press Esc to exit",
                   content_x + content_w / 2 - 9 * CHAR_STEP, icon_cy + 104,
                   DA_TEXT_DIM, DA_BG_SECONDARY, CHAR_STEP);

    /* 状态栏 */
    da_draw_statusbar(&g_ac, "Browser | Stub - No network stack", (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    /* 鼠标光标 */
    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
}

__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("browser", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("browser", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    da_cursor_init(&g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
    da_mouse_init();
    redraw_all();

    for (;;) {
        int need_redraw = 0;

        /* 鼠标轮询 */
        if (da_mouse_poll(&g_mouse, &g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h))
            need_redraw = 1;

        /* 键盘轮询 */
        u8 st = inb(0x64);
        if ((st & 1) && !(st & 0x20)) {
            u8 data = inb(0x60);
            if (!(data & 0x80) && data == 0x01) {
                da_slog("browser", "exit");
                return;
            }
        }

        if (need_redraw) {
            da_cursor_restore(&g_ac, &g_cursor);
            redraw_all();
        }
        __asm__("pause");
    }
}
