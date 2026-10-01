/* wm.c — 窗口管理（win_*）+ 窗口装饰绘制（M0 自 main.c 平移）
 */
#include "desktop.h"

/* ---- 全局状态 ---- */
desktop_window g_windows[MAX_WINDOWS];
int g_win_count = 0;
static int g_next_win_id = 1;
int g_focused_win = -1;

/* 拖拽状态 */
int g_dragging = 0;
int g_drag_win = -1;
int g_drag_off_x, g_drag_off_y;

/* ============================================================
 *  窗口管理
 * ============================================================ */

desktop_window *win_find(int id) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) return &g_windows[i];
    }
    return 0;
}

desktop_window *win_find_at(int mx, int my) {
    for (int i = g_win_count - 1; i >= 0; i--) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->minimized) continue;
        if (w->ws != g_ws_cur) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
            return w;
        }
    }
    return 0;
}

static void win_bring_to_front(desktop_window *w) {
    int idx = -1;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == w->id) { idx = i; break; }
    }
    if (idx < 0 || idx == g_win_count - 1) return;
    desktop_window tmp = g_windows[idx];
    for (int i = idx; i < g_win_count - 1; i++) {
        g_windows[i] = g_windows[i+1];
    }
    g_windows[g_win_count - 1] = tmp;
}

void win_focus(desktop_window *w) {
    int id = w->id;   /* bring_to_front 会重排数组，先保存 id 避免悬空指针 */
    win_bring_to_front(w);
    for (int i = 0; i < g_win_count; i++)
        g_windows[i].focused = (g_windows[i].id == id);
    g_focused_win = id;
    if (g_win_count > 0) g_windows[g_win_count - 1].minimized = 0;
}

desktop_window *win_create(int app_id, const char *title, int x, int y, int w, int h) {
    if (g_win_count >= MAX_WINDOWS) return 0;
    desktop_window *win = &g_windows[g_win_count++];
    int id = g_next_win_id++;
    win->id = id;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->visible = 1;
    win->focused = 1;
    win->minimized = 0;
    win->app_id = app_id;
    win->app_state = 0;
    win->closing = 0;
    win->dirty = 1;
    win->ws = g_ws_cur;
    for (int i = 0; i < 47 && title[i]; i++) win->title[i] = title[i];
    win->title[47] = 0;
    for (int i = 0; i < g_win_count - 1; i++) g_windows[i].focused = 0;
    g_focused_win = id;
    return win;
}

void win_destroy(desktop_window *w) {
    int idx = -1;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == w->id) { idx = i; break; }
    }
    if (idx < 0) return;

    /* 直接隐藏窗口（去掉关闭动画以避免卡顿和闪屏） */
    w->visible = 0;

    if (w->app_id >= 0 && w->app_id < g_app_count && g_apps[w->app_id].on_destroy && w->app_state) {
        g_apps[w->app_id].on_destroy(w->app_state);
    }
    for (int i = idx; i < g_win_count - 1; i++) g_windows[i] = g_windows[i+1];
    g_win_count--;
    if (g_focused_win == w->id) {
        g_focused_win = -1;
        for (int i = g_win_count - 1; i >= 0; i--) {
            if (g_windows[i].ws == g_ws_cur && g_windows[i].visible && !g_windows[i].minimized) {
                g_focused_win = g_windows[i].id;
                g_windows[i].focused = 1;
                break;
            }
        }
    }
}

/* 窗口标题栏高度（M2：Fluent 36px / KATE 28px） */
int win_titlebar_h(void) {
    return g_fluent ? 36 : KATE_TITLEBAR_H;
}

/* P5d: 关闭请求。运行中的 PE 窗口不能直接 win_destroy（run_windowed 阻塞中，
 * pump 持有窗口 id）——按 pe_window_host 协作契约 inject WM_CLOSE，
 * PE 自愿退出后由 launch_pe_app 收尾销毁。 */
void win_close_request(desktop_window *w) {
    if (g_pe_running && w->id == g_pe_win_id && g_pe_svc && g_pe_svc->inject_input) {
        g_pe_svc->inject_input(PE_WM_CLOSE, 0, 0, 0, 0);
        return;
    }
    win_destroy(w);
}

int ws_win_count(int ws) {
    int n = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_windows[i].ws == ws && g_windows[i].visible) n++;
    return n;
}

/* ============================================================
 *  窗口装饰绘制
 * ============================================================ */

/* M2：Fluent 窗口框架 —— 圆角 8、36px 标题栏、右上三钮、投影。
 * 窗口按钮命中区（WM_FLUENT_TB=36px 标题栏，三钮各 46×36）。 */
#define WM_FLUENT_TB   36
#define WM_BTN_W       46

static int wm_fluent_hit_btn(desktop_window *w, int mx, int my, int btn) {
    int bx = w->x + w->w - WM_BTN_W * btn;
    return mx >= bx && mx < bx + WM_BTN_W &&
           my >= w->y && my < w->y + WM_FLUENT_TB;
}

/* 静态状态：正在操作的三钮（0=无 1=min 2=max 3=close） */
static int g_wm_btn_hover = 0;

int win_hit_close_btn(desktop_window *w, int mx, int my) {
    if (g_fluent) return wm_fluent_hit_btn(w, mx, my, 3);
    int bx = w->x + w->w - CLOSE_BTN_OFFSET;
    int by = w->y + (KATE_TITLEBAR_H - CLOSE_BTN_SIZE) / 2;
    return mx >= bx && mx < bx + CLOSE_BTN_SIZE &&
           my >= by && my < by + CLOSE_BTN_SIZE;
}

int win_hit_min_btn(desktop_window *w, int mx, int my) {
    return g_fluent ? wm_fluent_hit_btn(w, mx, my, 1) : 0;
}

int win_hit_max_btn(desktop_window *w, int mx, int my) {
    return g_fluent ? wm_fluent_hit_btn(w, mx, my, 2) : 0;
}

static void draw_window_frame_fluent(desktop_window *w) {
    /* 投影 + 亚克力底（窗口主体用不透明深色，性能优先） */
    du_rect_outline(&g_fb, w->x - 2, w->y - 2, w->w + 4, w->h + 4,
                    du_blend(du_pixel_read(&g_fb, w->x - 2, w->y - 2), 0xFF000000u, 70), 10);
    du_fill_rounded_rect(&g_fb, w->x, w->y, w->w, w->h, 0xFF2B2B2Bu, 8);
    du_rect_outline(&g_fb, w->x, w->y, w->w, w->h,
                    w->focused ? 0xFF4A4A4Au : 0xFF3F3F3Fu, 8);

    /* 标题栏（36px，无强调色，仅底部分隔线） */
    du_fill_rect(&g_fb, w->x + 1, w->y + 1, w->w - 2, WM_FLUENT_TB - 1, 0xFF2B2B2Bu);
    du_divider_h(&g_fb, w->x + 1, w->y + WM_FLUENT_TB, w->w - 2, 0xFF3A3A3Au);
    du_draw_string(&g_fb, w->title,
                   w->x + 14,
                   w->y + (WM_FLUENT_TB - (i64)DU_ASCII_LINE_H) / 2,
                   w->focused ? 0xFFFFFFFFu : 0xFFC8C8C8u, 0xFF2B2B2Bu, DU_ASCII_STEP);

    /* 右上三钮：最小化 / 最大化 / 关闭（hover：前两钮浅灰，关闭钮红） */
    int by = w->y;
    int iy = by + WM_FLUENT_TB / 2;
    /* min */
    {
        int bx = w->x + w->w - WM_BTN_W * 3;
        if (g_wm_btn_hover == 1)
            du_fill_rect(&g_fb, bx, by, WM_BTN_W, WM_FLUENT_TB, 0xFF3A3A3Au);
        du_fill_rect(&g_fb, bx + 16, iy, 14, 2, 0xFFFFFFFFu);
    }
    /* max */
    {
        int bx = w->x + w->w - WM_BTN_W * 2;
        if (g_wm_btn_hover == 2)
            du_fill_rect(&g_fb, bx, by, WM_BTN_W, WM_FLUENT_TB, 0xFF3A3A3Au);
        du_rect_outline(&g_fb, bx + 17, iy - 7, 13, 13, 0xFFFFFFFFu, 1);
    }
    /* close：hover 红 #E81123 */
    {
        int bx = w->x + w->w - WM_BTN_W;
        if (g_wm_btn_hover == 3)
            du_fill_rect(&g_fb, bx, by, WM_BTN_W, WM_FLUENT_TB, 0xFFC42B1Cu);
        /* × 用两条 2px 对角线 */
        for (int i = 0; i < 11; i++) {
            du_pixel(&g_fb, bx + 18 + i, iy - 5 + i, 0xFFFFFFFFu);
            du_pixel(&g_fb, bx + 19 + i, iy - 5 + i, 0xFFFFFFFFu);
            du_pixel(&g_fb, bx + 18 + i, iy + 5 - i, 0xFFFFFFFFu);
            du_pixel(&g_fb, bx + 19 + i, iy + 5 - i, 0xFFFFFFFFu);
        }
    }
}

int win_hit_titlebar(desktop_window *w, int mx, int my) {
    int tb = g_fluent ? WM_FLUENT_TB : KATE_TITLEBAR_H;
    if (g_fluent && (win_hit_close_btn(w, mx, my) || win_hit_max_btn(w, mx, my) ||
                     win_hit_min_btn(w, mx, my)))
        return 0;   /* 三钮区域不算标题栏拖拽 */
    return mx >= w->x && mx < w->x + w->w &&
           my >= w->y && my < w->y + tb;
}

/* 窗口按钮 hover 状态跟踪（绘制前调用；btn 0=无） */
void wm_set_btn_hover(int btn) { g_wm_btn_hover = btn; }

/* 当前鼠标悬停的窗口按钮（0=无 1=min 2=max 3=close），-1=不在窗口上 */
int wm_detect_btn_hover(desktop_window *w, int mx, int my) {
    if (!w) return 0;
    if (win_hit_close_btn(w, mx, my)) return 3;
    if (win_hit_max_btn(w, mx, my)) return 2;
    if (win_hit_min_btn(w, mx, my)) return 1;
    return 0;
}

void draw_window_frame(desktop_window *w) {
    if (!w->visible || w->minimized) return;

    if (g_fluent) {
        draw_window_frame_fluent(w);
        return;
    }

    /* ---- Winux-Kate 风格（fluent=0 回退路径） ---- */
    u32 border = w->focused ? KS_BORDER_FOCUS : KS_BORDER;

    if (w->focused) {
        du_kate_glow_border(&g_fb, w->x, w->y, w->w, w->h, KS_ACCENT);
    }

    du_fill_rounded_rect(&g_fb, w->x, w->y, w->w, w->h, KS_BG_SECONDARY, 2);

    du_fill_rect_gradient(&g_fb, w->x + 1, w->y + 1, w->w - 2, KATE_TITLEBAR_H - 1,
                          0xFF0A3050u, KS_BG_SECONDARY);
    du_divider_h(&g_fb, w->x, w->y + KATE_TITLEBAR_H, w->w, KS_BORDER);

    du_fill_rect(&g_fb, w->x + 6, w->y + KATE_TITLEBAR_H / 2 - 2, 4, 4, KS_ACCENT2);

    du_draw_string(&g_fb, w->title,
                   w->x + 14,
                   w->y + (KATE_TITLEBAR_H - (i64)DU_ASCII_LINE_H) / 2 + 1,
                   KS_ACCENT, 0xFF0A3050u, DU_ASCII_STEP);

    int bx = w->x + w->w - CLOSE_BTN_OFFSET;
    int by = w->y + (KATE_TITLEBAR_H - CLOSE_BTN_SIZE) / 2;
    du_rect_outline(&g_fb, bx, by, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, KS_DANGER, 1);
    du_draw_string(&g_fb, "X", bx + 3, by - 1,
                   KS_DANGER, KS_BG_SECONDARY, DU_ASCII_STEP);

    du_rect_outline(&g_fb, w->x, w->y, w->w, w->h, border, 2);
}
