/* fluent_ui.h — Deshab "Win11 Fluent" 视觉组件库
 *
 * 设计目标：商业化 Windows 11 风格 —— 居中悬浮圆角任务栏、亚克力半透明
 * 弹层、圆角窗口、浅色/深色主题（见桌面重构计划第二节）。
 *
 * 分层：
 *   L1  wf_theme   主题令牌（色板 + 圆角），g_wf_light / g_wf_dark 预设
 *   L2  原语       du_wf_acrylic（读背景 alpha 混合圆角面板）、du_wf_shadow、
 *                  du_wf_line（Bresenham 带宽线）、du_wf_ring（圆环/半弧）
 *   L3  组件       按钮/图标钮/开关/滑块/列表项/卡片/文本框/滚动条/提示
 *   L4  图标       du_wf_icon 矢量线性图标（24 网格设计，2px 笔触）
 *
 * 约定（与 deshab_ui.h 相同）：包含本头前，编译单元须已定义 g_ascii
 * （include ascii_font.h 或 ascii_bitmaps.c）。所有组件为纯绘制函数，
 * hover/press 态由调用方检测并传入。
 *
 * 中文文本：本库组件标签为 ASCII；中文请调用方用 zh_draw_string +
 * 主题令牌色绘制（zhfont.h 在 CODE/desktop/，不进 UTSM 公共头）。
 */
#ifndef DESHAB_FLUENT_UI_H
#define DESHAB_FLUENT_UI_H

#include <utsm/deshab_ui.h>

/* ===================================================================
 *  L1 — 主题令牌 (wf_theme)
 * =================================================================== */

typedef struct {
    /* 亚克力面板 */
    du_u32 acrylic_tint;        /* 混入背景的 tint 色 */
    du_u32 acrylic_stroke;      /* 1px 边线 */
    du_u32 acrylic_solid;       /* 不透明回退底色（无 alpha 场景） */

    /* 文本 */
    du_u32 text_primary;
    du_u32 text_secondary;
    du_u32 text_dim;

    /* 强调色（Win11 accent） */
    du_u32 accent;
    du_u32 accent_hover;
    du_u32 accent_press;
    du_u32 on_accent;           /* accent 上的文字 */

    /* 控件 */
    du_u32 control_stroke;
    du_u32 window_border;
    du_u32 divider;
    du_u32 shadow;

    /* 语义色 */
    du_u32 success, warning, error;
    du_u32 close_hover;         /* 窗口关闭钮 hover 红 #E81123 */

    /* 圆角 */
    int radius_sm;              /* 4  按钮/输入框 */
    int radius_md;              /* 8  卡片/窗口 */
    int radius_lg;              /* 12 任务栏/开始菜单/弹层 */
} wf_theme;

/* 浅色主题（Win11 Light：底 #F3F3F3，accent #0067C0） */
static const wf_theme g_wf_light = {
    0xFFF3F3F3u,                /* acrylic_tint */
    0xFFEDEDEDu,                /* acrylic_stroke */
    0xFFF6F6F6u,                /* acrylic_solid */
    0xFF1A1A1Au, 0xFF5D5D5Du, 0xFF8A8A8Au,
    0xFF0067C0u, 0xFF0078D4u, 0xFF005A9Eu, 0xFFFFFFFFu,
    0xFFD6D6D6u,                /* control_stroke */
    0xFFE5E5E5u,                /* window_border */
    0xFFEDEDEDu,                /* divider */
    0x66000000u,                /* shadow */
    0xFF0F7B0Fu, 0xFF9D5D00u, 0xFFC42B1Cu,
    0xFFC42B1Cu,
    4, 8, 12,
};

/* 深色主题（Win11 Dark：底 #202020，accent #4CC2FF） */
static const wf_theme g_wf_dark = {
    0xFF202020u,
    0xFF3C3C3Cu,                /* acrylic_stroke */
    0xFF2B2B2Bu,
    0xFFFFFFFFu, 0xFFC8C8C8u, 0xFF9A9A9Au,
    0xFF4CC2FFu, 0xFF62CBFFu, 0xFF29A8E0u, 0xFF00293Cu,
    0xFF3C3C3Cu,                /* control_stroke */
    0xFF3F3F3Fu,                /* window_border */
    0xFF3A3A3Au,                /* divider */
    0x99000000u,                /* shadow */
    0xFF6CCB5Fu, 0xFFFCE100u, 0xFFFF99A4u,
    0xFFC42B1Cu,
    4, 8, 12,
};

/* ===================================================================
 *  L2 — 原语
 * =================================================================== */

/* 亚克力面板：逐像素读背景 → alpha 混合 tint → 圆角掩码。
 * 真实 backdrop blur 不可行，靠"高透 tint + 1px 边 + 壁纸层透出"模拟。
 * 仅用于弹层/任务栏（面积 <40% 屏，性能预算见计划第七节）。 */
static inline void du_wf_acrylic(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 w, du_i64 h,
                                 du_u32 alpha, du_u32 radius,
                                 const wf_theme *t) {
    du_i64 r = (du_i64)radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (du_i64 row = 0; row < h; row++) {
        for (du_i64 col = 0; col < w; col++) {
            du_i64 dx = 0, dy = 0;
            int corner = 0;
            if (col < r && row < r)                { dx = r - 1 - col; dy = r - 1 - row; corner = 1; }
            else if (col >= w - r && row < r)      { dx = col - (w - r); dy = r - 1 - row; corner = 1; }
            else if (col < r && row >= h - r)      { dx = r - 1 - col; dy = row - (h - r); corner = 1; }
            else if (col >= w - r && row >= h - r) { dx = col - (w - r); dy = row - (h - r); corner = 1; }
            if (corner && dx * dx + dy * dy > r * r) continue;
            du_u32 bg = du_pixel_read(ctx, x + col, y + row);
            du_pixel(ctx, x + col, y + row, du_blend(bg, t->acrylic_tint, alpha));
        }
    }
    du_rect_outline(ctx, x, y, w, h, t->acrylic_stroke, (du_u32)r);
}

/* 投影：外扩 2 圈递减 alpha 描边（比逐像素卷积便宜 100 倍） */
static inline void du_wf_shadow(du_context *ctx, du_i64 x, du_i64 y,
                                du_i64 w, du_i64 h, du_u32 radius,
                                const wf_theme *t) {
    du_u32 base = du_pixel_read(ctx, x, y + h + 2);
    for (int i = 2; i >= 1; i--) {
        du_u32 alpha = (i == 2) ? 40 : 70;
        du_rect_outline(ctx, x - i, y - i, w + 2 * i, h + 2 * i,
                        du_blend(base, t->shadow, alpha),
                        radius + (du_u32)i);
    }
}

/* 半透明控件蒙层（hover/press 态） */
static inline void du_wf_overlay(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 w, du_i64 h, du_u32 overlay) {
    du_u32 alpha = overlay >> 24;
    if (!alpha) return;
    for (du_i64 row = 0; row < h; row++)
        for (du_i64 col = 0; col < w; col++)
            du_pixel(ctx, x + col, y + row,
                     du_blend(du_pixel_read(ctx, x + col, y + row),
                              overlay, alpha));
}

/* Bresenham 直线（带厚度 w 像素） */
static inline void du_wf_line(du_context *ctx, du_i64 x0, du_i64 y0,
                              du_i64 x1, du_i64 y1, du_i64 w, du_u32 color) {
    du_i64 dx = (x1 > x0) ? x1 - x0 : x0 - x1;
    du_i64 dy = (y1 > y0) ? y1 - y0 : y0 - y1;
    du_i64 sx = (x0 < x1) ? 1 : -1;
    du_i64 sy = (y0 < y1) ? 1 : -1;
    du_i64 err = dx - dy;
    du_i64 half = w / 2;
    for (;;) {
        du_fill_rect(ctx, x0 - half, y0 - half, w, w, color);
        if (x0 == x1 && y0 == y1) break;
        du_i64 e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* 圆环：中心 (cx,cy)，外半径 r，厚度 s。
 *   half : 0 全环，1 仅上半（dy<=0），-1 仅下半 */
static inline void du_wf_ring(du_context *ctx, du_i64 cx, du_i64 cy,
                              du_i64 r, du_i64 s, int half, du_u32 color) {
    for (du_i64 dy = -r; dy <= r; dy++) {
        if (half == 1 && dy > 0) continue;
        if (half == -1 && dy < 0) continue;
        for (du_i64 dx = -r; dx <= r; dx++) {
            du_i64 d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - s) * (r - s))
                du_pixel(ctx, cx + dx, cy + dy, color);
        }
    }
}

/* ===================================================================
 *  L3 — 组件
 * =================================================================== */

/* 按钮态 */
typedef enum { WF_BTN_NORMAL = 0, WF_BTN_HOVER, WF_BTN_PRESS } wf_btn_state;

/* 标准按钮：圆角 4，三态底色，ASCII 标签居中（accent=1 为强调按钮） */
static inline void du_wf_button(du_context *ctx, du_i64 x, du_i64 y, du_i64 w,
                                const char *label, const wf_theme *t,
                                wf_btn_state st, int accent) {
    du_i64 h = 32;
    du_u32 bg = accent ? t->accent : t->acrylic_solid;
    du_u32 fg = accent ? t->on_accent : t->text_primary;
    if (accent) {
        du_fill_rounded_rect(ctx, x, y, w, h, bg, 4);
        if (st != WF_BTN_NORMAL)
            du_wf_overlay(ctx, x + 1, y + 1, w - 2, h - 2,
                          (st == WF_BTN_PRESS) ? 0x44FFFFFFu : 0x22FFFFFFu);
    } else {
        du_fill_rounded_rect(ctx, x, y, w, h, bg, 4);
        du_rect_outline(ctx, x, y, w, h, t->control_stroke, 4);
        if (st != WF_BTN_NORMAL)
            du_wf_overlay(ctx, x + 1, y + 1, w - 2, h - 2,
                          (st == WF_BTN_PRESS) ? 0x33000000u : 0x22000000u);
    }
    du_i64 len = 0;
    while (label[len]) len++;
    du_draw_string(ctx, label,
                   x + (w - len * (du_i64)DU_ASCII_STEP) / 2,
                   y + (h - (du_i64)DU_ASCII_CELL_H) / 2,
                   fg, bg, DU_ASCII_STEP);
}

/* 图标按钮：圆角 4 悬停/按下蒙层（图标由调用方叠加绘制） */
static inline void du_wf_icon_button(du_context *ctx, du_i64 x, du_i64 y,
                                     du_i64 size, const wf_theme *t,
                                     wf_btn_state st) {
    (void)t;
    if (st == WF_BTN_NORMAL) return;
    du_wf_overlay(ctx, x, y, size, size,
                  (st == WF_BTN_PRESS) ? 0x33000000u : 0x22000000u);
    du_rect_outline(ctx, x, y, size, size, 0x33000000u, 4);
}

/* 开关：轨道 40×20 圆角 full + 滑块 */
static inline void du_wf_toggle(du_context *ctx, du_i64 x, du_i64 y,
                                int on, const wf_theme *t) {
    du_u32 track = on ? t->accent : 0xFF8A8A8Au;
    du_fill_rounded_rect(ctx, x, y, 40, 20, track, 10);
    if (on) du_fill_rounded_rect(ctx, x + 20, y + 2, 16, 16, 0xFFFFFFFFu, 8);
    else    du_fill_rounded_rect(ctx, x + 4,  y + 2, 16, 16, 0xFFFFFFFFu, 8);
}

/* 滑块：细轨 4px 圆头 + 圆形拇指。rect_out 返回可拖拽轨道命中区。 */
static inline void du_wf_slider(du_context *ctx, du_i64 x, du_i64 y,
                                du_i64 w, int val, const wf_theme *t,
                                int rect_out[4]) {
    du_i64 ty = y + 8;
    du_fill_rounded_rect(ctx, x, ty, w, 4, t->control_stroke, 2);
    du_i64 fw = w * val / 100;
    if (fw > 2) du_fill_rounded_rect(ctx, x, ty, fw, 4, t->accent, 2);
    du_i64 kx = x + fw - 8;
    if (kx < x) kx = x;
    if (kx > x + w - 16) kx = x + w - 16;
    du_fill_rounded_rect(ctx, kx, y, 16, 16, t->accent, 8);
    if (rect_out) {
        rect_out[0] = (int)x; rect_out[1] = (int)y - 2;
        rect_out[2] = (int)w; rect_out[3] = 20;
    }
}

/* 列表项：32px 行高，hover/selected 蒙层 + 左侧 3px accent 条 */
static inline void du_wf_list_item(du_context *ctx, du_i64 x, du_i64 y,
                                   du_i64 w, const char *label,
                                   const wf_theme *t, int hover, int selected) {
    if (selected)      du_wf_overlay(ctx, x, y, w, 32, 0x28000000u);
    else if (hover)    du_wf_overlay(ctx, x, y, w, 32, 0x1A000000u);
    if (selected) du_fill_rect(ctx, x, y + 8, 3, 16, t->accent);
    du_draw_string(ctx, label, x + 12,
                   y + (32 - (du_i64)DU_ASCII_CELL_H) / 2,
                   selected ? t->text_primary : t->text_secondary,
                   0, DU_ASCII_STEP);
}

/* 卡片：圆角 8 + 投影 + 亚克力 */
static inline void du_wf_card(du_context *ctx, du_i64 x, du_i64 y,
                              du_i64 w, du_i64 h, du_u32 alpha,
                              const wf_theme *t) {
    du_wf_shadow(ctx, x, y, w, h, (du_u32)t->radius_md, t);
    du_wf_acrylic(ctx, x, y, w, h, alpha, (du_u32)t->radius_md, t);
}

/* 文本框（搜索框）：圆角 4，聚焦时 accent 底边线 + 光标 */
static inline void du_wf_textbox(du_context *ctx, du_i64 x, du_i64 y,
                                 du_i64 w, const char *text, int cursor_blink,
                                 const wf_theme *t, int focused) {
    du_i64 h = 36;
    du_fill_rounded_rect(ctx, x, y, w, h, t->acrylic_solid, 4);
    du_rect_outline(ctx, x, y, w, h, focused ? t->accent : t->control_stroke, 4);
    if (focused) du_fill_rect(ctx, x + 4, y + h - 2, w - 8, 2, t->accent);
    du_u32 fg = text[0] ? t->text_primary : t->text_dim;
    du_draw_string(ctx, text, x + 12, y + (h - (du_i64)DU_ASCII_CELL_H) / 2,
                   fg, t->acrylic_solid, DU_ASCII_STEP);
    if (focused && cursor_blink) {
        du_i64 len = 0;
        while (text[len]) len++;
        du_fill_rect(ctx, x + 12 + len * (du_i64)DU_ASCII_STEP + 2,
                     y + 9, 2, (du_i64)DU_ASCII_CELL_H, t->accent);
    }
}

/* 滚动条：4px 覆盖式 */
static inline void du_wf_scrollbar(du_context *ctx, du_i64 x, du_i64 y,
                                   du_i64 h, int pos_0_100, int ratio_0_100,
                                   const wf_theme *t) {
    if (ratio_0_100 >= 100) return;
    du_i64 thumb_h = h * ratio_0_100 / 100;
    if (thumb_h < 24) thumb_h = 24;
    du_i64 max_travel = h - thumb_h;
    du_i64 ty = y + max_travel * pos_0_100 / 100;
    du_fill_rounded_rect(ctx, x, ty, 4, thumb_h, t->text_dim, 2);
}

/* 提示（tooltip）：小圆角不透明底 */
static inline void du_wf_tooltip(du_context *ctx, du_i64 x, du_i64 y,
                                 const char *label, const wf_theme *t) {
    du_i64 len = 0;
    while (label[len]) len++;
    du_i64 w = len * (du_i64)DU_ASCII_STEP + 16;
    du_fill_rounded_rect(ctx, x, y, w, 28, t->acrylic_solid, 4);
    du_rect_outline(ctx, x, y, w, 28, t->control_stroke, 4);
    du_draw_string(ctx, label, x + 8, y + (28 - (du_i64)DU_ASCII_CELL_H) / 2,
                   t->text_primary, t->acrylic_solid, DU_ASCII_STEP);
}

/* ===================================================================
 *  L4 — 矢量图标（24 网格设计，线性 2px 笔触，Win11 Segoe 风格）
 *
 *  所有图标以 (x, y) 为左上角、size 为边长绘制，按 u = size/24 缩放
 *  （u 为最小单位，笔触 s = 2u）。
 * =================================================================== */

typedef enum {
    WF_ICON_START = 1,     /* 四方块（开始） */
    WF_ICON_SEARCH,        /* 放大镜 */
    WF_ICON_NETWORK,       /* WiFi 扇形 */
    WF_ICON_VOLUME,        /* 喇叭 */
    WF_ICON_POWER,         /* 电源 */
    WF_ICON_TASKMGR,       /* 折线图 */
    WF_ICON_SETTINGS,      /* 齿轮（简化） */
    WF_ICON_FOLDER,        /* 文件夹 */
    WF_ICON_TERMINAL,      /* 终端 >_ */
    WF_ICON_EDITOR,        /* 文档横线 */
    WF_ICON_CALC,          /* 计算器格子 */
    WF_ICON_BROWSER,       /* 地球 */
    WF_ICON_TASKVIEW,      /* 双矩形 */
    WF_ICON_WIDGETS,       /* 四小件 */
    WF_ICON_NOTIFY,        /* 铃铛 */
    WF_ICON_CLOSE,         /* × */
    WF_ICON_MIN,           /* – */
    WF_ICON_MAX,           /* □ */
    WF_ICON_RESTORE,       /* 双层 □ */
    WF_ICON_USER,          /* 人形 */
} wf_icon_id;

static inline void du_wf_icon(du_context *ctx, wf_icon_id id,
                              du_i64 x, du_i64 y, du_i64 size, du_u32 color) {
    du_i64 u = size / 24;
    if (u < 1) u = 1;
    du_i64 s = u * 2;       /* 笔触宽 */
    switch (id) {
    case WF_ICON_START:
        du_fill_rounded_rect(ctx, x + 3 * u,  y + 3 * u,  8 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 13 * u, y + 3 * u,  8 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 3 * u,  y + 13 * u, 8 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 13 * u, y + 13 * u, 8 * u, 8 * u, color, (du_u32)u);
        break;
    case WF_ICON_SEARCH:
        du_wf_ring(ctx, x + 10 * u, y + 10 * u, 7 * u, s, 0, color);
        du_wf_line(ctx, x + 15 * u, y + 15 * u, x + 20 * u, y + 20 * u, s, color);
        break;
    case WF_ICON_NETWORK:
        du_fill_rounded_rect(ctx, x + 10 * u, y + 17 * u, 3 * u, 3 * u, color, (du_u32)u);
        du_wf_ring(ctx, x + 11 * u, y + 19 * u, 6 * u, s, 1, color);
        du_wf_ring(ctx, x + 11 * u, y + 21 * u, 10 * u, s, 1, color);
        break;
    case WF_ICON_VOLUME:
        /* 喇叭主体：梯形 */
        du_fill_rect(ctx, x + 3 * u, y + 9 * u, 4 * u, 6 * u, color);
        du_wf_line(ctx, x + 7 * u, y + 9 * u, x + 11 * u, y + 5 * u, s, color);
        du_wf_line(ctx, x + 11 * u, y + 5 * u, x + 11 * u, y + 19 * u, s, color);
        du_wf_line(ctx, x + 11 * u, y + 19 * u, x + 7 * u, y + 15 * u, s, color);
        /* 声波弧 */
        du_wf_ring(ctx, x + 11 * u, y + 12 * u, 7 * u, s, 1, color);
        break;
    case WF_ICON_POWER:
        du_fill_rect(ctx, x + 11 * u, y + 3 * u, s, 8 * u, color);
        du_wf_ring(ctx, x + 12 * u, y + 13 * u, 8 * u, s, 0, color);
        break;
    case WF_ICON_TASKMGR:
        du_rect_outline(ctx, x + 2 * u, y + 3 * u, 20 * u, 18 * u, color, (du_u32)u);
        du_wf_line(ctx, x + 5 * u,  y + 15 * u, x + 9 * u,  y + 10 * u, s, color);
        du_wf_line(ctx, x + 9 * u,  y + 10 * u, x + 13 * u, y + 13 * u, s, color);
        du_wf_line(ctx, x + 13 * u, y + 13 * u, x + 19 * u, y + 6 * u,  s, color);
        break;
    case WF_ICON_SETTINGS:
        du_wf_ring(ctx, x + 12 * u, y + 12 * u, 5 * u, s, 0, color);
        for (int i = 0; i < 8; i++) {
            /* 8 个齿：每 45° 一个短径向线 */
            du_i64 ax[8] = {0, 1, 1, 1, 0, -1, -1, -1};
            du_i64 ay[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
            du_wf_line(ctx, x + 12 * u + ax[i] * 7 * u, y + 12 * u + ay[i] * 7 * u,
                       x + 12 * u + ax[i] * 10 * u, y + 12 * u + ay[i] * 10 * u,
                       s, color);
        }
        break;
    case WF_ICON_FOLDER:
        du_fill_rect(ctx, x + 3 * u, y + 6 * u, 7 * u, 3 * u, color);
        du_rect_outline(ctx, x + 3 * u, y + 6 * u, 18 * u, 12 * u, color, (du_u32)u);
        du_fill_rect(ctx, x + 3 * u, y + 11 * u, 18 * u, u, color);
        break;
    case WF_ICON_TERMINAL:
        du_rect_outline(ctx, x + 2 * u, y + 4 * u, 20 * u, 16 * u, color, (du_u32)u);
        du_wf_line(ctx, x + 6 * u, y + 9 * u, x + 10 * u, y + 12 * u, u + 1, color);
        du_wf_line(ctx, x + 10 * u, y + 12 * u, x + 6 * u, y + 15 * u, u + 1, color);
        du_fill_rect(ctx, x + 12 * u, y + 15 * u, 5 * u, u + 1, color);
        break;
    case WF_ICON_EDITOR:
        du_rect_outline(ctx, x + 5 * u, y + 2 * u, 14 * u, 20 * u, color, (du_u32)u);
        du_fill_rect(ctx, x + 8 * u, y + 8 * u, 8 * u, u, color);
        du_fill_rect(ctx, x + 8 * u, y + 12 * u, 8 * u, u, color);
        du_fill_rect(ctx, x + 8 * u, y + 16 * u, 5 * u, u, color);
        break;
    case WF_ICON_CALC:
        du_rect_outline(ctx, x + 4 * u, y + 2 * u, 16 * u, 20 * u, color, (du_u32)u);
        du_rect_outline(ctx, x + 7 * u, y + 5 * u, 10 * u, 4 * u, color, (du_u32)u);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                du_fill_rect(ctx, x + (7 + c * 4) * u, y + (12 + r * 3) * u,
                             2 * u, 2 * u, color);
        break;
    case WF_ICON_BROWSER:
        du_wf_ring(ctx, x + 12 * u, y + 12 * u, 9 * u, s, 0, color);
        du_fill_rect(ctx, x + 3 * u, y + 11 * u, 18 * u, s, color);
        du_wf_ring(ctx, x + 12 * u, y + 12 * u, 5 * u, u, 0, 0); /* 清理中圈 */
        du_wf_line(ctx, x + 8 * u, y + 4 * u, x + 8 * u, y + 20 * u, u, color);
        du_wf_line(ctx, x + 16 * u, y + 4 * u, x + 16 * u, y + 20 * u, u, color);
        break;
    case WF_ICON_TASKVIEW:
        du_rect_outline(ctx, x + 3 * u, y + 5 * u, 13 * u, 11 * u, color, (du_u32)u);
        du_rect_outline(ctx, x + 9 * u, y + 8 * u, 12 * u, 11 * u, color, (du_u32)u);
        break;
    case WF_ICON_WIDGETS:
        du_fill_rounded_rect(ctx, x + 3 * u,  y + 3 * u,  8 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 13 * u, y + 3 * u,  8 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 3 * u,  y + 13 * u, 5 * u, 8 * u, color, (du_u32)u);
        du_fill_rounded_rect(ctx, x + 10 * u, y + 13 * u, 11 * u, 8 * u, color, (du_u32)u);
        break;
    case WF_ICON_NOTIFY:
        /* 铃铛：拱顶 + 两侧斜线 + 底线 + 铃锤 */
        du_wf_ring(ctx, x + 12 * u, y + 14 * u, 8 * u, s, 1, color);
        du_wf_line(ctx, x + 4 * u, y + 14 * u, x + 4 * u, y + 18 * u, s, color);
        du_wf_line(ctx, x + 20 * u, y + 14 * u, x + 20 * u, y + 18 * u, s, color);
        du_fill_rect(ctx, x + 3 * u, y + 19 * u, 18 * u, s, color);
        du_fill_rounded_rect(ctx, x + 10 * u, y + 21 * u, 4 * u, 3 * u, color, (du_u32)u);
        break;
    case WF_ICON_CLOSE:
        du_wf_line(ctx, x + 6 * u, y + 6 * u, x + 18 * u, y + 18 * u, s, color);
        du_wf_line(ctx, x + 18 * u, y + 6 * u, x + 6 * u, y + 18 * u, s, color);
        break;
    case WF_ICON_MIN:
        du_fill_rect(ctx, x + 6 * u, y + 11 * u, 12 * u, s, color);
        break;
    case WF_ICON_MAX:
        du_rect_outline(ctx, x + 5 * u, y + 5 * u, 14 * u, 14 * u, color, (du_u32)u);
        break;
    case WF_ICON_RESTORE:
        du_rect_outline(ctx, x + 4 * u, y + 8 * u, 11 * u, 11 * u, color, (du_u32)u);
        du_fill_rect(ctx, x + 8 * u, y + 5 * u, 12 * u, s, color);
        du_fill_rect(ctx, x + 18 * u, y + 5 * u, s, 6 * u, color);
        du_fill_rect(ctx, x + 12 * u, y + 8 * u, 8 * u, u, color);
        break;
    case WF_ICON_USER:
        du_wf_ring(ctx, x + 12 * u, y + 8 * u, 4 * u, s, 0, color);
        du_wf_ring(ctx, x + 12 * u, y + 26 * u, 9 * u, s, 1, color);
        break;
    default:
        du_rect_outline(ctx, x + 4 * u, y + 4 * u, 16 * u, 16 * u, color, (du_u32)u);
        break;
    }
}

#endif /* DESHAB_FLUENT_UI_H */
