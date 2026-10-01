/* taskbar.c — Win11 Fluent 居中悬浮任务栏 + 系统托盘（M2）
 *
 * 布局（深色主题 g_wf_dark）：
 *   主任务栏：48px 高，距底 8px，水平居中悬浮，acrylic 圆角 12
 *     [开始 40×40] [任务视图 40×40] [运行窗口按钮 44×44 ...]
 *     运行中 = 底部 3×16 accent 指示条；聚焦 = 指示条加宽 6px + 微亮蒙层
 *   托盘组（右下角独立悬浮，同视觉）：
 *     [网络 40] [音量 40] [电源 40] [两行时钟：HH:MM / M-D]
 *
 * 点击路由 taskbar_click() 由 shell.c 在 Fluent 分支调用。
 * 开始菜单 M3 接入；电源 M2 直接退出（M7 扩展菜单）。
 */
#include "desktop.h"
#include "ascii_font.h"          /* 顺序敏感：先于 fluent_ui.h */
#include "../UTSM/include/utsm/fluent_ui.h"
#include "zhfont.h"
#include "textures.h"

/* ---- 命中矩形（绘制时更新，点击时使用） ---- */
int g_tb2_start_rect[4];
int g_tb2_tv_rect[4];
int g_tb2_app_rect[MAX_WINDOWS][4];
int g_tb2_tray_rect[4];          /* 托盘组整体 */
int g_tb2_net_rect[4];
int g_tb2_vol_rect[4];
int g_tb2_pwr_rect[4];
int g_tb2_clk_rect[4];

#define TB_H      48
#define TB_BOTTOM_GAP 8
#define TB_BTN    40             /* 开始/任务视图/托盘钮边长 */
#define TB_APPBTN 44             /* 应用钮边长 */
#define TB_PAD    8              /* 条内边距 */

static const wf_theme *tb_theme(void) { return &g_wf_dark; }

/* ---- 开始菜单 logo（textures/startMenuLogo.rgba，加载失败回退字形） ---- */
static int g_smlogo_ok = 0;

typedef int (*tb_read_fn)(const char *path, unsigned char *dst,
                          unsigned int cap, unsigned int *out_size);

int taskbar_init(tb_read_fn read_to) {
    u32 sz = 0;
    if (!read_to) return -1;
    if (read_to("system/deshab64/desktop/textures/startMenuLogo.rgba",
                (u8 *)SMLOGO_ADDR, SMLOGO_W * SMLOGO_H * 4u, &sz) != 0)
        return -2;
    if (sz < SMLOGO_W * SMLOGO_H * 4u) return -3;
    g_smlogo_ok = 1;
    return 0;
}

/* app_id → Fluent 图标 */
static wf_icon_id tb_app_icon(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return WF_ICON_TERMINAL;
    const char *n = g_apps[app_id].name;
    if (!n) return WF_ICON_FOLDER;
    if (n[0]=='s' && n[1]=='h') return WF_ICON_TERMINAL;      /* shell */
    if (n[0]=='c' && n[1]=='m' && n[2]=='d') return WF_ICON_TERMINAL;
    if (n[0]=='e' && n[1]=='d') return WF_ICON_EDITOR;        /* editor */
    if (n[0]=='p' && n[1]=='r') return WF_ICON_EDITOR;        /* proedit */
    if (n[0]=='f' && n[1]=='i') return WF_ICON_FOLDER;        /* fileman */
    if (n[0]=='c' && n[1]=='a') return WF_ICON_CALC;          /* calc */
    if (n[0]=='b' && n[1]=='r') return WF_ICON_BROWSER;       /* browser */
    if (n[0]=='s' && n[1]=='e') return WF_ICON_SETTINGS;      /* settings */
    if (n[0]=='v' || n[0]=='k') return WF_ICON_BROWSER;       /* vscode/kde */
    return WF_ICON_FOLDER;
}

static wf_btn_state tb_hover_state(const int r[4]) {
    if (g_mouse_x >= r[0] && g_mouse_x < r[0] + r[2] &&
        g_mouse_y >= r[1] && g_mouse_y < r[1] + r[3])
        return WF_BTN_HOVER;
    return WF_BTN_NORMAL;
}

/* ============================================================
 *  绘制
 * ============================================================ */

void taskbar_draw(void) {
    const wf_theme *t = tb_theme();
    int W = (int)g_fb_w, H = (int)g_fb_h;
    int by = H - TB_H - TB_BOTTOM_GAP;

    /* ---- 主任务栏内容宽度 ---- */
    int nbtn = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_windows[i].visible && g_windows[i].ws == g_ws_cur) nbtn++;
    int content_w = TB_PAD + TB_BTN + 4 + TB_BTN + 6 + nbtn * (TB_APPBTN + 4) + TB_PAD - 4;
    int bx = (W - content_w) / 2;

    /* ---- 托盘组宽度 ---- */
    const char *time_s = "00:00";
    const char *date_s = "0/0";
    char tbuf[8], dbuf[10];
    {
        int yy, mo, dd, hh, mi, ss;
        rtc_read(&yy, &mo, &dd, &hh, &mi, &ss);
        tbuf[0] = '0' + hh / 10; tbuf[1] = '0' + hh % 10; tbuf[2] = ':';
        tbuf[3] = '0' + mi / 10; tbuf[4] = '0' + mi % 10; tbuf[5] = 0;
        time_s = tbuf;
        int dl = 0;
        if (mo >= 10) dbuf[dl++] = '0' + mo / 10;
        dbuf[dl++] = '0' + mo % 10; dbuf[dl++] = '/';
        if (dd >= 10) dbuf[dl++] = '0' + dd / 10;
        dbuf[dl++] = '0' + dd % 10; dbuf[dl] = 0;
        date_s = dbuf;
    }
    int clk_w = zh_string_width(time_s, 0) + 8;
    int tray_w = TB_PAD + TB_BTN + 2 + TB_BTN + 2 + TB_BTN + 6 + clk_w + TB_PAD;
    int tx = W - tray_w - 8;

    /* ---- 主任务栏面板 ---- */
    du_wf_acrylic(&g_fb, bx, by, content_w, TB_H, 216, t->radius_lg, t);
    int x = bx + TB_PAD;

    /* 开始按钮（logo：用户 textures/startMenuLogo，缺失回退 "D" 字形） */
    g_tb2_start_rect[0] = x; g_tb2_start_rect[1] = by + (TB_H - TB_BTN) / 2;
    g_tb2_start_rect[2] = TB_BTN; g_tb2_start_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, by + (TB_H - TB_BTN) / 2, TB_BTN, t,
                      tb_hover_state(g_tb2_start_rect));
    if (g_smlogo_ok) {
        tex_draw_scaled(&g_fb, (const u8 *)SMLOGO_ADDR, SMLOGO_W, SMLOGO_H,
                        x + 6, by + (TB_H - TB_BTN) / 2 + 6, 28, 28, 256);
    } else {
        du_draw_string(&g_fb, "D", x + TB_BTN / 2 - 4,
                       by + (TB_H - TB_BTN) / 2 + TB_BTN / 2 - 8,
                       t->accent, 0, DU_ASCII_STEP * 2);
    }
    x += TB_BTN + 4;

    /* 任务视图按钮 */
    g_tb2_tv_rect[0] = x; g_tb2_tv_rect[1] = by + (TB_H - TB_BTN) / 2;
    g_tb2_tv_rect[2] = TB_BTN; g_tb2_tv_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, by + (TB_H - TB_BTN) / 2, TB_BTN, t,
                      tb_hover_state(g_tb2_tv_rect));
    du_wf_icon(&g_fb, WF_ICON_TASKVIEW, x + (TB_BTN - 20) / 2,
               by + (TB_H - TB_BTN) / 2 + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 6;

    /* 运行窗口按钮（当前工作区，数组序 = z 序） */
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->ws != g_ws_cur) {
            g_tb2_app_rect[i][2] = 0;
            continue;
        }
        int byy = by + (TB_H - TB_APPBTN) / 2;
        g_tb2_app_rect[i][0] = x; g_tb2_app_rect[i][1] = byy;
        g_tb2_app_rect[i][2] = TB_APPBTN; g_tb2_app_rect[i][3] = TB_APPBTN;
        wf_btn_state st = tb_hover_state(g_tb2_app_rect[i]);
        if (w->focused) du_wf_overlay(&g_fb, x, byy, TB_APPBTN, TB_APPBTN, 0x22FFFFFFu);
        else if (st == WF_BTN_HOVER) du_wf_overlay(&g_fb, x, byy, TB_APPBTN, TB_APPBTN, 0x22FFFFFFu);
        du_wf_icon(&g_fb, tb_app_icon(w->app_id), x + (TB_APPBTN - 22) / 2,
                   byy + (TB_APPBTN - 22) / 2, 22, t->text_primary);
        /* 运行指示条：底部 accent，聚焦加宽 */
        int ind_w = w->focused ? 18 : 12;
        du_fill_rounded_rect(&g_fb, x + (TB_APPBTN - ind_w) / 2,
                             byy + TB_APPBTN - 6, ind_w, 3, t->accent, 1);
        x += TB_APPBTN + 4;
    }

    /* ---- 托盘组面板 ---- */
    du_wf_acrylic(&g_fb, tx, by, tray_w, TB_H, 216, t->radius_lg, t);
    x = tx + TB_PAD;
    int iy = by + (TB_H - TB_BTN) / 2;

    g_tb2_net_rect[0] = x; g_tb2_net_rect[1] = iy;
    g_tb2_net_rect[2] = TB_BTN; g_tb2_net_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_net_rect));
    du_wf_icon(&g_fb, WF_ICON_NETWORK, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 2;

    g_tb2_vol_rect[0] = x; g_tb2_vol_rect[1] = iy;
    g_tb2_vol_rect[2] = TB_BTN; g_tb2_vol_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_vol_rect));
    du_wf_icon(&g_fb, WF_ICON_VOLUME, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 2;

    g_tb2_pwr_rect[0] = x; g_tb2_pwr_rect[1] = iy;
    g_tb2_pwr_rect[2] = TB_BTN; g_tb2_pwr_rect[3] = TB_BTN;
    du_wf_icon_button(&g_fb, x, iy, TB_BTN, t, tb_hover_state(g_tb2_pwr_rect));
    du_wf_icon(&g_fb, WF_ICON_POWER, x + (TB_BTN - 20) / 2, iy + (TB_BTN - 20) / 2, 20, t->text_primary);
    x += TB_BTN + 6;

    /* 两行时钟（16px 中文位图，行高 18） */
    g_tb2_clk_rect[0] = x; g_tb2_clk_rect[1] = by;
    g_tb2_clk_rect[2] = clk_w; g_tb2_clk_rect[3] = TB_H;
    {
        int ty1 = by + (TB_H - 36) / 2;
        zh_draw_string(&g_fb, time_s, x, ty1, t->text_primary, 0);
        zh_draw_string(&g_fb, date_s, x, ty1 + 18, t->text_dim, 0);
    }
    g_tb2_tray_rect[0] = tx; g_tb2_tray_rect[1] = by;
    g_tb2_tray_rect[2] = tray_w; g_tb2_tray_rect[3] = TB_H;
}

/* ============================================================
 *  点击路由（shell.c Fluent 分支调用；返回 1 = 已消费）
 * ============================================================ */

int taskbar_click(int mx, int my) {
    /* 主任务栏 */
    if (rect_hit(g_tb2_start_rect, mx, my)) {
        slog("[taskbar] start button (start menu: M3)");
        return 1;
    }
    if (rect_hit(g_tb2_tv_rect, mx, my)) {
        g_taskview = 1;
        return 1;
    }
    for (int i = 0; i < g_win_count; i++) {
        if (!g_tb2_app_rect[i][2] || !rect_hit(g_tb2_app_rect[i], mx, my))
            continue;
        /* 聚焦窗口；已聚焦则最小化（Win11 行为） */
        if (g_windows[i].focused && !g_windows[i].minimized) {
            g_windows[i].minimized = 1;
            g_focused_win = -1;
            g_windows[i].focused = 0;
        } else {
            g_windows[i].minimized = 0;
            win_focus(&g_windows[i]);
        }
        return 1;
    }
    /* 托盘 */
    if (rect_hit(g_tb2_pwr_rect, mx, my)) {
        g_quit = 1;                       /* M7：电源菜单（睡眠/重启/关机） */
        return 1;
    }
    if (rect_hit(g_tb2_net_rect, mx, my)) {
        slog("[taskbar] network (quick settings: M6)");
        return 1;
    }
    if (rect_hit(g_tb2_vol_rect, mx, my)) {
        slog("[taskbar] volume (quick settings: M6)");
        return 1;
    }
    if (rect_hit(g_tb2_clk_rect, mx, my)) {
        slog("[taskbar] clock (notification center: M6)");
        return 1;
    }
    /* 命中面板空白处：吞掉点击（不落到桌面） */
    if (rect_hit(g_tb2_tray_rect, mx, my)) return 1;
    {
        int W = (int)g_fb_w, H = (int)g_fb_h;
        int by = H - TB_H - TB_BOTTOM_GAP;
        if (my >= by && my < by + TB_H) {
            int nbtn = 0;
            for (int i = 0; i < g_win_count; i++)
                if (g_windows[i].visible && g_windows[i].ws == g_ws_cur) nbtn++;
            int content_w = TB_PAD + TB_BTN + 4 + TB_BTN + 6 + nbtn * (TB_APPBTN + 4) + TB_PAD - 4;
            int bx = (W - content_w) / 2;
            if (mx >= bx && mx < bx + content_w) return 1;
        }
    }
    return 0;
}
