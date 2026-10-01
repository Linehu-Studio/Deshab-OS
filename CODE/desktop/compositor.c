/* compositor.c — 脏矩形、双缓冲 flip、全屏重绘（M0 自 main.c 平移）
 * M2：Fluent 重绘路径（壁纸背景 + 无 topbar + 悬浮任务栏 overlay）。
 */
#include "desktop.h"
#include "ascii_font.h"
#include "../UTSM/include/utsm/fluent_ui.h"
#include "wallpaper.h"

/* ---- 全局状态 ---- */
static dirty_rect g_dirty_rects[MAX_DIRTY_RECTS];
int g_dirty_count = 0;
int g_full_redraw = 1;  /* 初始全屏重绘 */

/* 标记脏区域 */
__attribute__((unused))
void mark_dirty(int x, int y, int w, int h) {
    if (g_dirty_count >= MAX_DIRTY_RECTS) {
        /* 溢出时合并为全屏重绘 */
        g_full_redraw = 1;
        return;
    }
    g_dirty_rects[g_dirty_count].x = x;
    g_dirty_rects[g_dirty_count].y = y;
    g_dirty_rects[g_dirty_count].w = w;
    g_dirty_rects[g_dirty_count].h = h;
    g_dirty_rects[g_dirty_count].active = 1;
    g_dirty_count++;
}

/* 合并所有脏矩形为一个包围盒 */
void merge_dirty_rects(int *out_x, int *out_y, int *out_w, int *out_h) {
    if (g_dirty_count == 0) {
        *out_x = *out_y = *out_w = *out_h = 0;
        return;
    }
    int min_x = (int)g_fb_w, min_y = (int)g_fb_h;
    int max_x = 0, max_y = 0;
    for (int i = 0; i < g_dirty_count; i++) {
        if (!g_dirty_rects[i].active) continue;
        int x1 = g_dirty_rects[i].x;
        int y1 = g_dirty_rects[i].y;
        int x2 = x1 + g_dirty_rects[i].w;
        int y2 = y1 + g_dirty_rects[i].h;
        if (x1 < min_x) min_x = x1;
        if (y1 < min_y) min_y = y1;
        if (x2 > max_x) max_x = x2;
        if (y2 > max_y) max_y = y2;
    }
    *out_x = min_x;
    *out_y = min_y;
    *out_w = max_x - min_x;
    *out_h = max_y - min_y;
}

/* 清除所有脏矩形 */
void clear_dirty_rects(void) {
    g_dirty_count = 0;
    g_full_redraw = 0;
}

/* ---- 双缓冲 flip ---- */

/* 将 sprite buffer 完整复制到真实 framebuffer */
void flip_buffer(void) {
    if (!g_real_fb) return;
    u64 total = g_fb_h * g_fb_pitch / 4;
    u32 *dst = (u32 *)g_real_fb;
    u32 *src = (u32 *)SPRITE_BUF_ADDR;
    for (u64 i = 0; i < total; i++) dst[i] = src[i];
}

/* 仅复制指定矩形区域（用于鼠标移动时的局部更新） */
void flip_rect(int x, int y, int w, int h) {
    if (!g_real_fb) return;
    for (int r = 0; r < h; r++) {
        int ry = y + r;
        if (ry < 0 || (u64)ry >= g_fb_h) continue;
        u32 *dst = (u32 *)(g_real_fb + (u64)ry * g_fb_pitch);
        u32 *src = (u32 *)((u8 *)SPRITE_BUF_ADDR + (u64)ry * g_fb_pitch);
        for (int c = 0; c < w; c++) {
            int rx = x + c;
            if (rx < 0 || (u64)rx >= g_fb_w) continue;
            dst[rx] = src[rx];
        }
    }
}

/* 非阻塞延时：动画期间持续轮询鼠标，保持光标响应。 */
__attribute__((unused))
void anim_delay_poll_mouse(u32 ms) {
    for (u32 i = 0; i < ms; i++) {
        for (volatile u32 j = 0; j < 50000; j++) {
            __asm__ volatile("pause");
        }
        if (ps2_mouse_poll()) {
            int old_x = g_cursor_old_x, old_y = g_cursor_old_y;
            cursor_restore_bg();
            flip_rect(old_x, old_y, CURSOR_SIZE, CURSOR_SIZE);
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
            flip_rect(g_mouse_x, g_mouse_y, CURSOR_SIZE, CURSOR_SIZE);
        }
    }
}

/* ============================================================
 *  全屏重绘
 * ============================================================ */

/* 全局亮度（BRIGHT 滑块真实调暗）
 * 性能优化：仅在亮度变化时处理，避免每帧全屏遍历 */
static void apply_brightness(void) {
    /* 快速退出：亮度100%无需调整 */
    if (g_bright >= 100) {
        g_bright_applied = 100;
        return;
    }

    /* 快速退出：亮度未变化，跳过处理 */
    if (g_bright == g_bright_applied) return;

    /* 记录当前应用的亮度值 */
    g_bright_applied = g_bright;

    u32 f = (u32)g_bright;
    for (u64 y = 0; y < g_fb_h; y++) {
        u32 *line = (u32 *)((u8 *)g_fb.fb + y * g_fb_pitch);
        for (u64 x = 0; x < g_fb_w; x++) {
            u32 c = line[x];
            u32 b = ((c >> 16) & 0xFF) * f / 100;
            u32 g = ((c >> 8) & 0xFF) * f / 100;
            u32 r = (c & 0xFF) * f / 100;
            line[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

static void redraw_all_fluent(void);

void redraw_all(void) {
    if (g_fluent) {
        redraw_all_fluent();
        return;
    }

    /* ---- KATE 路径（fluent=0 回退） ---- */
    /* 深空黑渐变背景 */
    du_fill_bg_gradient(&g_fb, KS_BG_PRIMARY, KS_BG_TERTIARY);

    /* 顶栏 */
    draw_topbar();

    /* 页面 */
    if (g_page == 1) draw_dashboard();
    else if (g_page == 2) draw_ide();
    else if (g_page == 3) draw_desktop_page();
    else {
        int idx = -1;
        for (int i = 0; i < g_cpage_count; i++)
            if (g_cpages[i].id == g_page) { idx = i; break; }
        if (idx >= 0) draw_custom_page(idx);
        else g_page = 1;
    }

    /* 任务视图 overlay */
    if (g_taskview) draw_taskview();

    /* 右键菜单（最顶） */
    if (g_ctx_open) draw_ctx_menu();

    /* 亮度调整 */
    apply_brightness();

    /* 鼠标光标 */
    cursor_save_bg(g_mouse_x, g_mouse_y);
    cursor_draw(g_mouse_x, g_mouse_y);
}

/* M2 Fluent 重绘：壁纸背景（缺失回退深色渐变）+ 页面 + 悬浮任务栏 */
static void redraw_all_fluent(void) {
    /* 背景：壁纸层缓存直拷，失败回退渐变 */
    {
        wp_u32 *wp = wallpaper_layer();
        if (wp) {
            for (u64 y = 0; y < g_fb_h; y++) {
                u32 *line = (u32 *)((u8 *)g_fb.fb + y * g_fb_pitch);
                const u32 *srcline = wp + y * (u64)g_fb_w;
                for (u64 x = 0; x < g_fb_w; x++) line[x] = srcline[x];
            }
        } else {
            du_fill_bg_gradient(&g_fb, 0xFF0A0E1Cu, 0xFF16264Au);
        }
    }

    /* 页面（无 topbar；DESKTOP 页绘制窗口，任务栏由 taskbar_draw 叠加） */
    if (g_page == 1) draw_dashboard();
    else if (g_page == 2) draw_ide();
    else if (g_page == 3) draw_desktop_page();
    else {
        int idx = -1;
        for (int i = 0; i < g_cpage_count; i++)
            if (g_cpages[i].id == g_page) { idx = i; break; }
        if (idx >= 0) draw_custom_page(idx);
        else g_page = 3;   /* Fluent 默认页 = DESKTOP */
    }

    /* 任务视图 overlay */
    if (g_taskview) draw_taskview();

    /* 右键菜单（最顶） */
    if (g_ctx_open) draw_ctx_menu();

    /* 悬浮任务栏（DESKTOP 页；其他页也显示以保持导航） */
    taskbar_draw();

    /* 亮度调整 */
    apply_brightness();

    /* 鼠标光标 */
    cursor_save_bg(g_mouse_x, g_mouse_y);
    cursor_draw(g_mouse_x, g_mouse_y);
}
