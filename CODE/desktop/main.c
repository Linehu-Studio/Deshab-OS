/* Deshab Desktop Manager — 图形桌面环境
 *
 * 功能：窗口管理、任务栏、桌面图标、事件路由、应用加载
 * 风格：Sealed Arc 视觉设计语言
 * 输入：PS/2 键盘 + PS/2 鼠标
 */

#include "../UTSM/include/utsm/dsk.h"

/* block_read 函数类型（从 kernel_api + 0xA8 获取） */
typedef int (*desktop_block_read_fn)(void *ctx, unsigned long long lba, unsigned int count, void *buffer);

/* 先包含 ascii_bitmaps.c（定义 g_ascii），再包含 deshab_ui.h（引用 g_ascii） */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static void sputc(char c) {
    for (unsigned int i=0; i<100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static void slog(const char *s) { swrite("[desktop] "); swrite(s); swrite("\n"); }

/* 前向声明 */
static void redraw_all(void);
static int  ps2_mouse_poll(void);

/* ============================================================
 *  常量
 * ============================================================ */

#define MAX_WINDOWS        8
#define TASKBAR_H          40
#define TITLEBAR_H         28
#define BORDER_W           2
#define DESKTOP_ICON_W     80
#define DESKTOP_ICON_H     72
#define ICON_GUTTER_X      16
#define ICON_GUTTER_Y      16
#define CURSOR_SIZE        24

/* ============================================================
 *  事件系统
 * ============================================================ */

typedef enum {
    EV_MOUSE_MOVE = 1,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_KEY_DOWN,
    EV_KEY_UP,
} ev_type;

typedef struct {
    ev_type type;
    int mx, my;          /* 鼠标全局坐标 */
    int button;          /* 0=左, 1=右 */
    u8  scancode;
    int shift, ctrl, alt;
} desktop_event;

/* ============================================================
 *  窗口
 * ============================================================ */

typedef struct desktop_window {
    int    id;
    int    x, y, w, h;
    int    visible;
    int    focused;
    int    minimized;
    char   title[48];
    int    app_id;          /* 关联应用 ID */
    void  *app_state;       /* 应用私有状态 */
    int    closing;         /* 关闭中标记 */
    int    dirty;           /* 需要重绘 */
} desktop_window;

/* ============================================================
 *  应用描述符
 * ============================================================ */

/* 前向声明 */
struct desktop_window;
typedef struct desktop_window desktop_win;

typedef struct app_ctx {
    du_context  *fb;
    desktop_win *win;
    int          client_x, client_y, client_w, client_h;
    int          should_exit;
    desktop_block_read_fn block_read;
    const void  *kernel_api;
} app_ctx;

typedef void *(*app_create_fn)(app_ctx *ctx);
typedef void  (*app_event_fn)(void *state, app_ctx *ctx, desktop_event *ev);
typedef void  (*app_draw_fn)(void *state, app_ctx *ctx);
typedef void  (*app_destroy_fn)(void *state);

typedef struct {
    const char *name;
    const char *display_name;
    u32         default_w, default_h;
    app_create_fn  on_create;
    app_event_fn   on_event;
    app_draw_fn    on_draw;
    app_destroy_fn on_destroy;
} app_descriptor;

/* ============================================================
 *  全局状态
 * ============================================================ */

static du_context g_fb;
static u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch;
static const void *g_kernel_api;
static desktop_block_read_fn g_block_read;

/* 鼠标状态 */
static int g_mouse_x = 400, g_mouse_y = 300;
static int g_mouse_btn = 0;  /* bit0=左键 */
static u8  g_mouse_buf[3];
static int g_mouse_idx = 0;
static int g_mouse_has_pkt = 0;

/* 光标背景保存 */
static u32 g_cursor_save[CURSOR_SIZE * CURSOR_SIZE];
static int g_cursor_saved = 0;
static int g_cursor_old_x = -1, g_cursor_old_y = -1;

/* 键盘状态 */
static int g_shift = 0, g_ctrl = 0, g_alt = 0;
static int g_e0 = 0;

/* 窗口列表 */
static desktop_window g_windows[MAX_WINDOWS];
static int g_win_count = 0;
static int g_next_win_id = 1;
static int g_focused_win = -1;

/* 拖拽状态 */
static int g_dragging = 0;
static int g_drag_win = -1;
static int g_drag_off_x, g_drag_off_y;

/* 应用表 */
#define MAX_APPS 8
static app_descriptor g_apps[MAX_APPS];
static int g_app_count = 0;

/* 桌面图标 */
typedef struct {
    int    app_id;
    int    x, y;           /* 图标位置 */
    const char *label;
} desktop_icon;

#define MAX_ICONS 8
static desktop_icon g_icons[MAX_ICONS];
static int g_icon_count = 0;

/* ============================================================
 *  箭头光标形状（24×24，1=前景 0=透明）
 * ============================================================ */

static const u8 cursor_shape[24][24] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* ============================================================
 *  应用图标形状（32×32 像素数据，用程序化方式生成）
 * ============================================================ */

/* 绘制终端图标（32×32）到指定位置 */
static void draw_icon_terminal(du_context *ctx, int cx, int cy) {
    u32 bg = DP_ABYSS_700;
    u32 fg = DP_SEAL_300;
    u32 border = DP_SEAL_500;
    /* 圆角矩形背景 */
    du_fill_rounded_rect(ctx, cx, cy, 32, 32, bg, 6);
    du_rect_outline(ctx, cx, cy, 32, 32, border, 6);
    /* >_ 符号 */
    du_fill_rect(ctx, cx+5, cy+10, 8, 2, fg);
    du_fill_rect(ctx, cx+5, cy+10, 2, 6, fg);
    du_fill_rect(ctx, cx+13, cy+12, 2, 2, fg);
    du_fill_rect(ctx, cx+17, cy+16, 6, 2, fg);
}

/* 绘制编辑器图标（32×32） */
static void draw_icon_editor(du_context *ctx, int cx, int cy) {
    u32 bg = DP_ABYSS_700;
    u32 fg = DP_SEAL_500;
    u32 border = DP_SEAL_300;
    du_fill_rounded_rect(ctx, cx, cy, 32, 32, bg, 6);
    du_rect_outline(ctx, cx, cy, 32, 32, border, 6);
    /* 文本行 */
    for (int i = 0; i < 5; i++) {
        int lw = (i == 0) ? 20 : (i == 3 ? 12 : 16);
        du_fill_rect(ctx, cx+6, cy+6+i*5, lw, 2, fg);
    }
}

/* 绘制文件夹图标（32×32） */
static void draw_icon_folder(du_context *ctx, int cx, int cy) {
    u32 bg = DP_ARC_500;
    u32 fg = DP_ARC_300;
    u32 border = DP_ARC_700;
    du_fill_rounded_rect(ctx, cx, cy, 32, 32, bg, 6);
    du_rect_outline(ctx, cx, cy, 32, 32, border, 6);
    /* 文件夹标签 */
    du_fill_rect(ctx, cx+4, cy+6, 12, 3, fg);
    /* 文件夹主体 */
    du_fill_rect(ctx, cx+4, cy+12, 24, 2, border);
    du_fill_rect(ctx, cx+4, cy+14, 24, 12, fg);
}

/* 绘制计算器图标（32×32） */
static void draw_icon_calc(du_context *ctx, int cx, int cy) {
    u32 bg = DP_ABYSS_700;
    u32 fg = DP_WARNING;
    u32 border = DP_WARNING;
    du_fill_rounded_rect(ctx, cx, cy, 32, 32, bg, 6);
    du_rect_outline(ctx, cx, cy, 32, 32, border, 6);
    /* 屏幕 */
    du_fill_rect(ctx, cx+4, cy+4, 24, 6, DP_ABYSS_900);
    du_fill_rect(ctx, cx+22, cy+5, 4, 4, fg);
    /* 按键网格 */
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            du_fill_rect(ctx, cx+4+c*8, cy+14+r*6, 6, 4, DP_NEUTRAL_400);
        }
    }
}

/* ============================================================
 *  光标管理
 * ============================================================ */

static void cursor_save_bg(int mx, int my) {
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            int x = mx + c, y = my + r;
            if (x >= 0 && (u64)x < g_fb_w && y >= 0 && (u64)y < g_fb_h) {
                u32 *line = (u32 *)((u8 *)g_fb.fb + (u64)y * g_fb.pitch);
                g_cursor_save[r * CURSOR_SIZE + c] = line[x];
            } else {
                g_cursor_save[r * CURSOR_SIZE + c] = 0;
            }
        }
    }
    g_cursor_saved = 1;
    g_cursor_old_x = mx;
    g_cursor_old_y = my;
}

static void cursor_restore_bg(void) {
    if (!g_cursor_saved) return;
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            int x = g_cursor_old_x + c, y = g_cursor_old_y + r;
            if (x >= 0 && (u64)x < g_fb_w && y >= 0 && (u64)y < g_fb_h) {
                u32 *line = (u32 *)((u8 *)g_fb.fb + (u64)y * g_fb.pitch);
                line[x] = g_cursor_save[r * CURSOR_SIZE + c];
            }
        }
    }
    g_cursor_saved = 0;
}

static void cursor_draw(int mx, int my) {
    for (int r = 0; r < CURSOR_SIZE; r++) {
        for (int c = 0; c < CURSOR_SIZE; c++) {
            if (cursor_shape[r][c]) {
                du_pixel(&g_fb, mx + c, my + r, DS_DARK_TEXT_PRIMARY);
            }
        }
    }
}

/* 非阻塞延时：动画期间持续轮询鼠标，保持光标响应。
 * 将 ms 个 50000-iter pause 块拆开，每块结束后轮询一次鼠标。
 * 若鼠标有新数据包，立即擦除旧光标、在新位置保存背景并重绘光标。 */
static void anim_delay_poll_mouse(u32 ms) {
    for (u32 i = 0; i < ms; i++) {
        for (volatile u32 j = 0; j < 50000; j++) {
            __asm__ volatile("pause");
        }
        if (ps2_mouse_poll()) {
            cursor_restore_bg();
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
        }
    }
}

/* ============================================================
 *  窗口管理
 * ============================================================ */

static desktop_window *win_find(int id) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) return &g_windows[i];
    }
    return 0;
}

static desktop_window *win_find_at(int mx, int my) {
    /* 从后往前搜索（后面的窗口在上面） */
    for (int i = g_win_count - 1; i >= 0; i--) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->minimized) continue;
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

static desktop_window *win_create(int app_id, const char *title, int x, int y, int w, int h) {
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
    /* 拷贝标题 */
    for (int i = 0; i < 47 && title[i]; i++) win->title[i] = title[i];
    win->title[47] = 0;
    /* 取消其他窗口焦点 */
    for (int i = 0; i < g_win_count - 1; i++) g_windows[i].focused = 0;
    g_focused_win = id;
    return win;
}

static void win_destroy(desktop_window *w) {
    int idx = -1;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == w->id) { idx = i; break; }
    }
    if (idx < 0) return;

    /* Window close animation - shrinking rectangle */
    {
        int wx = w->x, wy = w->y, ww = w->w, wh = w->h;
        w->visible = 0;
        for (int step = 5; step >= 0; step--) {
            int scale = (step + 1) * 100 / 6;
            int aw = ww * scale / 100;
            int ah = wh * scale / 100;
            int ax = wx + (ww - aw) / 2;
            int ay = wy + (wh - ah) / 2;
            cursor_restore_bg();
            redraw_all();
            /* redraw_all 末尾已绘制光标，这里先擦除以保证保存的背景干净（不含光标像素） */
            cursor_restore_bg();
            du_rect_outline(&g_fb, ax, ay, aw, ah, DS_DARK_ACCENT, DU_RADIUS_SM);
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
            anim_delay_poll_mouse(16);
        }
    }

    /* 调用应用的 on_destroy */
    if (w->app_id >= 0 && w->app_id < g_app_count && g_apps[w->app_id].on_destroy && w->app_state) {
        g_apps[w->app_id].on_destroy(w->app_state);
    }
    for (int i = idx; i < g_win_count - 1; i++) g_windows[i] = g_windows[i+1];
    g_win_count--;
    if (g_focused_win == w->id) {
        g_focused_win = g_win_count > 0 ? g_windows[g_win_count-1].id : -1;
        if (g_focused_win >= 0) win_find(g_focused_win)->focused = 1;
    }
}

static int win_hit_titlebar(desktop_window *w, int mx, int my) {
    return mx >= w->x && mx < w->x + w->w &&
           my >= w->y && my < w->y + TITLEBAR_H;
}

static int win_hit_close_btn(desktop_window *w, int mx, int my) {
    int bx = w->x + w->w - 28;
    int by = w->y + 4;
    return mx >= bx && mx < bx + 24 && my >= by && my < by + 20;
}

/* ============================================================
 *  窗口装饰绘制
 * ============================================================ */

static void draw_window_frame(desktop_window *w) {
    if (!w->visible || w->minimized) return;
    u32 titlebar_bg = w->focused ? DP_ABYSS_600 : DP_ABYSS_800;
    u32 border_col = w->focused ? DS_DARK_BORDER_FOCUS : DS_DARK_BORDER;

    /* 发光效果（焦点窗口） */
    if (w->focused) {
        du_rect_glow(&g_fb, w->x-2, w->y-2, w->w+4, w->h+4,
                     DS_DARK_BORDER_FOCUS, 10, 3);
    }

    /* 窗口主体背景 */
    du_fill_rounded_rect(&g_fb, w->x, w->y, w->w, w->h,
                          DS_DARK_BG_SECONDARY, DU_RADIUS_LG);

    /* 标题栏 */
    du_fill_rect(&g_fb, w->x + BORDER_W, w->y + BORDER_W,
                 w->w - 2*BORDER_W, TITLEBAR_H - BORDER_W, titlebar_bg);

    /* 标题文字 */
    du_draw_string(&g_fb, w->title,
                   w->x + (i64)DU_SPACE_SM,
                   w->y + (TITLEBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_PRIMARY, titlebar_bg, DU_ASCII_STEP);

    /* 关闭按钮 */
    int bx = w->x + w->w - 28;
    int by = w->y + 4;
    du_fill_rounded_rect(&g_fb, bx, by, 24, 20, DP_ERROR, DU_RADIUS_SM);
    du_draw_string(&g_fb, "X", bx + 8, by + 1, DP_NEUTRAL_900, DP_ERROR, DU_ASCII_STEP);

    /* 边框 */
    du_rect_outline(&g_fb, w->x, w->y, w->w, w->h, border_col, DU_RADIUS_LG);

    /* 标题栏底线 */
    du_fill_rect(&g_fb, w->x + BORDER_W, w->y + TITLEBAR_H,
                 w->w - 2*BORDER_W, 1, border_col);
}

/* ============================================================
 *  任务栏绘制
 * ============================================================ */

static void draw_taskbar(void) {
    i64 tb_y = (i64)g_fb_h - TASKBAR_H;
    du_fill_rect(&g_fb, 0, tb_y, (i64)g_fb_w, TASKBAR_H, DP_ABYSS_800);
    /* 顶部强调线 */
    du_fill_rect(&g_fb, 0, tb_y, (i64)g_fb_w, 2, DS_DARK_ACCENT);

    /* Start 按钮 */
    du_fill_rounded_rect(&g_fb, (i64)DU_SPACE_SM, tb_y + 4, 48, 32,
                          DP_ABYSS_600, DU_RADIUS_SM);
    du_draw_string(&g_fb, "D",
                   (i64)DU_SPACE_SM + 18, tb_y + 8,
                   DS_DARK_ACCENT_LIGHT, DP_ABYSS_600, DU_ASCII_STEP);

    /* 快速启动图标 */
    int qx = 64;
    for (int i = 0; i < g_app_count && i < 4; i++) {
        int icon_x = qx + i * 40;
        switch (i) {
        case 0: draw_icon_terminal(&g_fb, icon_x + 4, (int)tb_y + 4); break;
        case 1: draw_icon_editor(&g_fb, icon_x + 4, (int)tb_y + 4); break;
        case 2: draw_icon_folder(&g_fb, icon_x + 4, (int)tb_y + 4); break;
        case 3: draw_icon_calc(&g_fb, icon_x + 4, (int)tb_y + 4); break;
        }
    }

    /* 窗口按钮 */
    int wx = qx + 4 * 40 + (int)DU_SPACE_LG;
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (w->minimized) continue;
        u32 btn_bg = w->focused ? DS_DARK_ACCENT : DP_ABYSS_600;
        du_fill_rounded_rect(&g_fb, wx, tb_y + 4, 100, 32, btn_bg, DU_RADIUS_SM);
        /* Truncate title to fit button width (100px - 8px padding = ~11 chars) */
        char truncated[16];
        int max_chars = 11;
        int tlen = 0;
        while (w->title[tlen] && tlen < max_chars) { truncated[tlen] = w->title[tlen]; tlen++; }
        if (tlen == max_chars && w->title[tlen]) {
            /* Add ellipsis */
            if (tlen > 8) tlen = 8;
            truncated[tlen++] = '.';
            truncated[tlen++] = '.';
            truncated[tlen++] = '.';
        }
        truncated[tlen] = 0;
        du_draw_string(&g_fb, truncated, wx + 4, tb_y + 8,
                       DS_DARK_TEXT_PRIMARY, btn_bg, DU_ASCII_STEP);
        wx += 104;
        if (wx > (int)g_fb_w - 200) break;
    }

    /* 时钟 */
    u8 rtc_h = 0, rtc_m = 0;
    outb(0x70, 4); rtc_h = inb(0x71);
    outb(0x70, 2); rtc_m = inb(0x71);
    /* BCD → 二进制 */
    rtc_h = (u8)((rtc_h >> 4) * 10 + (rtc_h & 0xF));
    rtc_m = (u8)((rtc_m >> 4) * 10 + (rtc_m & 0xF));
    char clock_str[8];
    clock_str[0] = '0' + rtc_h / 10;
    clock_str[1] = '0' + rtc_h % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + rtc_m / 10;
    clock_str[4] = '0' + rtc_m % 10;
    clock_str[5] = 0;
    du_draw_string(&g_fb, clock_str,
                   (i64)g_fb_w - 5 * (i64)DU_ASCII_STEP - (i64)DU_SPACE_MD,
                   tb_y + (TASKBAR_H - (i64)DU_ASCII_CELL_H) / 2,
                   DS_DARK_TEXT_SECONDARY, DP_ABYSS_800, DU_ASCII_STEP);
}

/* ============================================================
 *  桌面图标绘制
 * ============================================================ */

static void draw_desktop_icons(void) {
    for (int i = 0; i < g_icon_count; i++) {
        desktop_icon *ic = &g_icons[i];
        int cx = ic->x + (DESKTOP_ICON_W - 32) / 2;  /* center icon in slot */
        int cy = ic->y;
        /* 根据应用 ID 画不同图标 */
        switch (ic->app_id) {
        case 0: draw_icon_terminal(&g_fb, cx, cy); break;
        case 1: draw_icon_editor(&g_fb, cx, cy); break;
        case 2: draw_icon_folder(&g_fb, cx, cy); break;
        case 3: draw_icon_calc(&g_fb, cx, cy); break;
        default: draw_icon_terminal(&g_fb, cx, cy); break;
        }
        /* 标签文字 */
        int label_len = 0;
        while (ic->label[label_len]) label_len++;
        int label_x = ic->x + (DESKTOP_ICON_W - label_len * (int)DU_ASCII_STEP) / 2;
        if (label_x < ic->x) label_x = ic->x;
        du_draw_string(&g_fb, ic->label, label_x, cy + 36,
                       DS_DARK_TEXT_PRIMARY, DS_DARK_BG_PRIMARY, DU_ASCII_STEP);
    }
}

/* ============================================================
 *  PS/2 鼠标处理
 * ============================================================ */

/* PS/2 控制器等待 — 输入缓冲区空（可写） */
static int ps2_wait_write(void) {
    for (int t = 0; t < 200000; t++) {
        if (!(inb(0x64) & 0x02)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* PS/2 控制器等待 — 输出缓冲区满（可读） */
static int ps2_wait_read(void) {
    for (int t = 0; t < 200000; t++) {
        if (inb(0x64) & 0x01) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* 排干输出缓冲区中所有残留数据（带超时） */
static void ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(0x64) & 0x01)) break;
        inb(0x60);
    }
}

static void ps2_mouse_init(void) {
    /* 1. 刷新固件/上一阶段留下的残留数据 */
    ps2_drain();

    /* 2. 启用 AUX（鼠标）端口 */
    ps2_wait_write();
    outb(0x64, 0xA8);

    /* 3. 读取控制器配置字节 */
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    u8 cfg = inb(0x60);

    /* 4. 设置配置：bit5=0(启用 AUX 时钟), bit1=1(启用 AUX IRQ12), bit6=1(Set2→Set1 转换) */
    cfg &= ~0x20; /* enable AUX clock */
    cfg |= 0x02;  /* enable AUX IRQ12 */
    cfg |= 0x40;  /* enable Set2→Set1 translation */

    /* 5. 写回控制器配置 */
    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, cfg);

    /* 6. 复位鼠标 (0xFF) — 响应为 ACK(0xFA) + 自检(0xAA) + 设备ID(0x00)，共 3 字节 */
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xFF);
    /* 排干所有复位响应字节（最多 8 字节，带超时） */
    for (int i = 0; i < 8; i++) {
        if (ps2_wait_read() != 0) break;
        inb(0x60);
    }

    /* 7. 启用数据流模式 (0xF4) — 响应为 ACK(0xFA) */
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF4);
    /* 排干 ACK */
    for (int i = 0; i < 4; i++) {
        if (ps2_wait_read() != 0) break;
        inb(0x60);
    }

    /* 8. 最终刷新：丢弃任何杂散字节，确保数据包从干净状态开始 */
    ps2_drain();

    /* 重置鼠标数据包解析状态 */
    g_mouse_idx = 0;
    g_mouse_has_pkt = 0;

    slog("mouse init ok");
}

static int ps2_mouse_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    if (!(st & 0x20)) return 0; /* 不是鼠标数据（键盘数据留给键盘处理） */
    u8 data = inb(0x60);

    g_mouse_buf[g_mouse_idx++] = data;
    if (g_mouse_idx < 3) return 0;
    g_mouse_idx = 0;

    /* 检查 bit3 同步 */
    if (!(g_mouse_buf[0] & 0x08)) return 0;

    int dx = (int)(i8)g_mouse_buf[1];
    int dy = (int)(i8)g_mouse_buf[2];
    /* (i8) 转换已处理符号位，无需再检查 sign bits */
    dy = -dy; /* PS/2 Y 向上为正，屏幕 Y 向下为正，需翻转 */

    g_mouse_x += dx;
    g_mouse_y += dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= (int)g_fb_w - CURSOR_SIZE) g_mouse_x = (int)g_fb_w - CURSOR_SIZE;
    if (g_mouse_y >= (int)g_fb_h - CURSOR_SIZE) g_mouse_y = (int)g_fb_h - CURSOR_SIZE;

    int old_btn = g_mouse_btn;
    g_mouse_btn = (g_mouse_buf[0] & 0x01) ? 1 : 0;
    /* Only signal redraw if something visual changed */
    if (dx == 0 && dy == 0 && g_mouse_btn == old_btn) return 0;
    g_mouse_has_pkt = 1;
    return 1;
}

/* ============================================================
 *  键盘扫描码 → ASCII
 * ============================================================ */

static char scan_to_ascii(u8 sc, int shift) {
    static const char normal[58] = {
        0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    static const char shifted[58] = {
        0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
        'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
        'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
    };
    if (sc >= 58) return 0;
    return shift ? shifted[sc] : normal[sc];
}

/* ============================================================
 *  内置应用：终端 (Bash)
 * ============================================================ */

#define TERM_MAX_COLS  80
#define TERM_MAX_ROWS  40
#define TERM_MAX_CHARS (TERM_MAX_COLS * TERM_MAX_ROWS)

typedef struct {
    u8  ch[TERM_MAX_CHARS];
    u32 fg[TERM_MAX_CHARS];
    int cols, rows;
    int cur_col, cur_row;
    char input_buf[256];
    int input_len, input_cursor;
    int prompt_len;
} bash_state;

static const char *BASH_PROMPT = "deshab# ";

static void *bash_on_create(app_ctx *ctx) {
    bash_state *s = (bash_state *)0x4000000; /* 固定地址分配，避免堆依赖 */
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        s->ch[i] = ' '; s->fg[i] = DS_DARK_TEXT_PRIMARY;
    }
    s->cols = (ctx->client_w - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_STEP;
    s->rows = (ctx->client_h - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_LINE_H;
    if (s->cols > TERM_MAX_COLS) s->cols = TERM_MAX_COLS;
    if (s->rows > TERM_MAX_ROWS) s->rows = TERM_MAX_ROWS;
    s->cur_col = 0; s->cur_row = 0;
    s->input_len = 0; s->input_cursor = 0;
    s->prompt_len = 0;
    while (BASH_PROMPT[s->prompt_len]) s->prompt_len++;

    /* 打印欢迎 */
    const char *welcome = "Deshab Bash v0.1\n输入 help 查看命令\n\n";
    while (*welcome) {
        if (*welcome == '\n') {
            s->cur_col = 0; s->cur_row++;
        } else {
            if (s->cur_row < s->rows && s->cur_col < s->cols) {
                int idx = s->cur_row * TERM_MAX_COLS + s->cur_col;
                s->ch[idx] = (u8)*welcome;
                s->fg[idx] = DS_DARK_TEXT_PRIMARY;
            }
            s->cur_col++;
        }
        welcome++;
    }
    return s;
}

static void bash_putc(bash_state *s, char c, u32 color) {
    if (c == '\n') { s->cur_col = 0; s->cur_row++; return; }
    if (s->cur_row >= s->rows) {
        /* 滚动 */
        for (int r = 1; r < s->rows; r++) {
            for (int c2 = 0; c2 < s->cols; c2++) {
                s->ch[(r-1)*TERM_MAX_COLS+c2] = s->ch[r*TERM_MAX_COLS+c2];
                s->fg[(r-1)*TERM_MAX_COLS+c2] = s->fg[r*TERM_MAX_COLS+c2];
            }
        }
        for (int c2 = 0; c2 < s->cols; c2++) {
            s->ch[(s->rows-1)*TERM_MAX_COLS+c2] = ' ';
        }
        s->cur_row = s->rows - 1;
    }
    if (s->cur_col >= s->cols) { s->cur_col = 0; s->cur_row++; }
    int idx = s->cur_row * TERM_MAX_COLS + s->cur_col;
    s->ch[idx] = (u8)c; s->fg[idx] = color;
    s->cur_col++;
}

static void bash_puts(bash_state *s, const char *str, u32 color) {
    while (*str) bash_putc(s, *str++, color);
}

static void bash_execute(bash_state *s, const char *cmd) {
    while (*cmd == ' ') cmd++;
    bash_puts(s, BASH_PROMPT, DS_DARK_PROMPT);
    bash_puts(s, cmd, DS_DARK_TEXT_PRIMARY);
    bash_putc(s, '\n', 0);

    if (*cmd == 0) return;
    if (cmd[0]=='h'&&cmd[1]=='e'&&cmd[2]=='l'&&cmd[3]=='p') {
        bash_puts(s, "  help    clear    echo    version\n", DP_SUCCESS);
        bash_puts(s, "  uname   date     about   reboot\n", DP_SUCCESS);
        bash_puts(s, "  halt    pwd      whoami  id\n", DP_SUCCESS);
    } else if (cmd[0]=='c'&&cmd[1]=='l'&&cmd[2]=='e'&&cmd[3]=='a'&&cmd[4]=='r') {
        for (int i = 0; i < TERM_MAX_CHARS; i++) { s->ch[i] = ' '; }
        s->cur_col = 0; s->cur_row = 0;
    } else if (cmd[0]=='v'&&cmd[1]=='e'&&cmd[2]=='r') {
        bash_puts(s, "Deshab OS v0.1.0\n", DP_SUCCESS);
    } else if (cmd[0]=='u'&&cmd[1]=='n'&&cmd[2]=='a') {
        bash_puts(s, "Deshab\n", DS_DARK_TEXT_PRIMARY);
    } else if (cmd[0]=='a'&&cmd[1]=='b'&&cmd[2]=='o'&&cmd[3]=='u'&&cmd[4]=='t') {
        bash_puts(s, "Deshab OS - SAS-R0 Kernel\n", DP_SEAL_300);
        bash_puts(s, "Sealed Arc UI Design\n", DP_ARC_300);
    } else if (cmd[0]=='e'&&cmd[1]=='c'&&cmd[2]=='h'&&cmd[3]=='o') {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        bash_puts(s, arg, DS_DARK_TEXT_PRIMARY);
        bash_putc(s, '\n', 0);
    } else if (cmd[0]=='p'&&cmd[1]=='w'&&cmd[2]=='d') {
        bash_puts(s, "/\n", DS_DARK_TEXT_PRIMARY);
    } else if (cmd[0]=='w'&&cmd[1]=='h'&&cmd[2]=='o') {
        bash_puts(s, "root\n", DS_DARK_TEXT_PRIMARY);
    } else if (cmd[0]=='i'&&cmd[1]=='d') {
        bash_puts(s, "uid=0(root) gid=0(root)\n", DS_DARK_TEXT_PRIMARY);
    } else if (cmd[0]=='r'&&cmd[1]=='e'&&cmd[2]=='b') {
        bash_puts(s, "Rebooting...\n", DS_DARK_PROMPT);
        outb(0x64, 0xFE);
        for(;;) __asm__("hlt");
    } else if (cmd[0]=='h'&&cmd[1]=='a'&&cmd[2]=='l'&&cmd[3]=='t') {
        bash_puts(s, "Halted.\n", DS_DARK_PROMPT);
        for(;;) __asm__("hlt");
    } else {
        bash_puts(s, "unknown: ", DP_ERROR);
        bash_puts(s, cmd, DP_ERROR);
        bash_putc(s, '\n', 0);
    }
}

static void bash_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    bash_state *s = (bash_state *)state;
    if (ev->type != EV_KEY_DOWN) return;
    u8 sc = ev->scancode;
    if (sc == 0x1C) { /* Enter */
        s->input_buf[s->input_len] = 0;
        bash_execute(s, s->input_buf);
        s->input_len = 0; s->input_cursor = 0;
    } else if (sc == 0x0E) { /* Backspace */
        if (s->input_cursor > 0) {
            for (int i = s->input_cursor-1; i < s->input_len-1; i++)
                s->input_buf[i] = s->input_buf[i+1];
            s->input_len--; s->input_cursor--;
        }
    } else {
        char c = scan_to_ascii(sc, ev->shift);
        if (c && c >= 32 && c <= 126 && s->input_len < 255) {
            for (int i = s->input_len; i > s->input_cursor; i--)
                s->input_buf[i] = s->input_buf[i-1];
            s->input_buf[s->input_cursor++] = c;
            s->input_len++;
        }
    }
    ctx->win->dirty = 1;
}

static void bash_on_draw(void *state, app_ctx *ctx) {
    bash_state *s = (bash_state *)state;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    int cw = ctx->client_w;
    int ch = ctx->client_h;

    /* 背景 */
    du_fill_rect(&g_fb, cx, cy, cw, ch, DS_DARK_BG_PRIMARY);

    /* 渲染所有字符 */
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
            int idx = r * TERM_MAX_COLS + c;
            if (s->ch[idx] == ' ') continue;
            du_draw_char(&g_fb, s->ch[idx],
                         cx + (int)DU_SPACE_SM + c * (int)DU_ASCII_STEP,
                         cy + (int)DU_SPACE_SM + r * (int)DU_ASCII_LINE_H,
                         s->fg[idx], DS_DARK_BG_PRIMARY);
        }
    }

    /* 绘制当前输入行 */
    int input_y = cy + (int)DU_SPACE_SM + s->cur_row * (int)DU_ASCII_LINE_H;
    /* 提示符 */
    du_draw_string(&g_fb, BASH_PROMPT,
                   cx + (int)DU_SPACE_SM, input_y,
                   DS_DARK_PROMPT, DS_DARK_BG_PRIMARY, DU_ASCII_STEP);
    /* 输入内容 */
    du_draw_string(&g_fb, s->input_buf,
                   cx + (int)DU_SPACE_SM + s->prompt_len * (int)DU_ASCII_STEP,
                   input_y,
                   DS_DARK_TEXT_PRIMARY, DS_DARK_BG_PRIMARY, DU_ASCII_STEP);

    /* 光标 */
    int cursor_x = cx + (int)DU_SPACE_SM + (s->prompt_len + s->input_cursor) * (int)DU_ASCII_STEP;
    du_fill_rect(&g_fb, cursor_x, input_y + (int)DU_ASCII_CELL_H - 3,
                 (int)DU_ASCII_CELL_W, 2, DS_DARK_CURSOR);
}

static void bash_on_destroy(void *state) {
    (void)state;
}

/* ============================================================
 *  内置应用：文本编辑器 (Editor)
 * ============================================================ */

#define EDITOR_BUF_SIZE 65536

typedef struct {
    char text[EDITOR_BUF_SIZE];
    int  text_len;
    int  cursor_pos;
    int  scroll_y;       /* 行偏移 */
    int  cursor_line;
    int  cursor_col;
    int  modified;
    char filename[64];
} editor_state;

static void editor_recalc_cursor(editor_state *s) {
    s->cursor_line = 0; s->cursor_col = 0;
    for (int i = 0; i < s->cursor_pos && i < s->text_len; i++) {
        if (s->text[i] == '\n') { s->cursor_line++; s->cursor_col = 0; }
        else s->cursor_col++;
    }
}

static int editor_line_start(editor_state *s, int line) {
    int l = 0;
    for (int i = 0; i < s->text_len; i++) {
        if (l == line) return i;
        if (s->text[i] == '\n') l++;
    }
    return s->text_len;
}

static void *editor_on_create(app_ctx *ctx) {
    editor_state *s = (editor_state *)0x5000000; /* 固定地址 */
    s->text_len = 0; s->cursor_pos = 0;
    s->scroll_y = 0; s->modified = 0;
    s->filename[0] = 0;

    /* 默认内容 */
    const char *sample = "# Welcome to Deshab Editor\n#\n\nStart typing here...\n";
    while (*sample && s->text_len < EDITOR_BUF_SIZE - 1) {
        s->text[s->text_len++] = *sample++;
    }
    s->cursor_pos = s->text_len;
    editor_recalc_cursor(s);

    /* 设置窗口标题 */
    du_draw_string(&g_fb, "", 0, 0, 0, 0, 0); /* 占位 */
    for (int i = 0; i < 63; i++) ctx->win->title[i] = 0;
    const char *t = "Editor - untitled";
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];

    (void)ctx;
    return s;
}

static void editor_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    editor_state *s = (editor_state *)state;
    if (ev->type != EV_KEY_DOWN) return;
    u8 sc = ev->scancode;
    int e0 = 0; /* TODO: 扩展键处理 */

    if (sc == 0x1C) { /* Enter */
        if (s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = '\n';
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    } else if (sc == 0x0E) { /* Backspace */
        if (s->cursor_pos > 0) {
            for (int i = s->cursor_pos - 1; i < s->text_len - 1; i++)
                s->text[i] = s->text[i+1];
            s->text_len--; s->cursor_pos--;
            s->modified = 1;
        }
    } else if (sc == 0x48) { /* Up */
        if (s->cursor_line > 0) {
            int prev_start = editor_line_start(s, s->cursor_line - 1);
            int col = s->cursor_col;
            s->cursor_pos = prev_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x50) { /* Down */
        if (s->cursor_line < 999) {
            int next_start = editor_line_start(s, s->cursor_line + 1);
            int col = s->cursor_col;
            s->cursor_pos = next_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x4B) { /* Left */
        if (s->cursor_pos > 0) s->cursor_pos--;
    } else if (sc == 0x4D) { /* Right */
        if (s->cursor_pos < s->text_len) s->cursor_pos++;
    } else if (sc == 0x47) { /* Home */
        s->cursor_pos = editor_line_start(s, s->cursor_line);
    } else if (sc == 0x4F) { /* End */
        s->cursor_pos = editor_line_start(s, s->cursor_line);
        while (s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n')
            s->cursor_pos++;
    } else {
        char c = scan_to_ascii(sc, ev->shift);
        if (c && c >= 32 && c <= 126 && s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = c;
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    }
    editor_recalc_cursor(s);
    ctx->win->dirty = 1;
    (void)e0;
}

static void editor_on_draw(void *state, app_ctx *ctx) {
    editor_state *s = (editor_state *)state;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    int cw = ctx->client_w;
    int ch = ctx->client_h;

    int line_num_w = 5 * (int)DU_ASCII_STEP + (int)DU_SPACE_SM;
    int text_area_x = cx + line_num_w;
    int text_area_w = cw - line_num_w;
    int visible_rows = ch / (int)DU_ASCII_LINE_H;
    int visible_cols = text_area_w / (int)DU_ASCII_STEP;

    /* 背景 */
    du_fill_rect(&g_fb, cx, cy, cw, ch, DS_DARK_BG_SECONDARY);

    /* 自动滚动 */
    if (s->cursor_line < s->scroll_y) s->scroll_y = s->cursor_line;
    if (s->cursor_line >= s->scroll_y + visible_rows) s->scroll_y = s->cursor_line - visible_rows + 1;

    /* 渲染可见行 */
    int line = 0;
    int pos = 0;
    for (int i = 0; i < s->text_len || line <= s->cursor_line; ) {
        if (line >= s->scroll_y + visible_rows) break;

        if (line >= s->scroll_y) {
            int screen_y = cy + (line - s->scroll_y) * (int)DU_ASCII_LINE_H;

            /* 行号 */
            char ln[8];
            ln[0] = '0' + ((line+1)/100)%10;
            ln[1] = '0' + ((line+1)/10)%10;
            ln[2] = '0' + (line+1)%10;
            ln[3] = 0;
            du_draw_string(&g_fb, ln, cx + 2, screen_y,
                           DS_DARK_TEXT_DIM, DS_DARK_BG_SECONDARY, DU_ASCII_STEP);

            /* 行号分隔线 */
            du_fill_rect(&g_fb, cx + line_num_w - 2, screen_y, 1,
                         (int)DU_ASCII_CELL_H, DS_DARK_DIVIDER);

            /* 行文本 */
            int col = 0;
            while (pos < s->text_len && s->text[pos] != '\n' && col < visible_cols) {
                du_draw_char(&g_fb, s->text[pos],
                             text_area_x + col * (int)DU_ASCII_STEP,
                             screen_y,
                             DS_DARK_TEXT_PRIMARY, DS_DARK_BG_SECONDARY);
                col++; pos++;
            }
            if (pos < s->text_len && s->text[pos] == '\n') pos++;
        } else {
            while (pos < s->text_len && s->text[pos] != '\n') pos++;
            if (pos < s->text_len) pos++;
        }
        line++;
        if (pos >= s->text_len && line > s->cursor_line) break;
    }

    /* 光标 */
    if (s->cursor_line >= s->scroll_y && s->cursor_line < s->scroll_y + visible_rows) {
        int cursor_y = cy + (s->cursor_line - s->scroll_y) * (int)DU_ASCII_LINE_H;
        int cursor_x = text_area_x + s->cursor_col * (int)DU_ASCII_STEP;
        du_fill_rect(&g_fb, cursor_x, cursor_y, 2, (int)DU_ASCII_CELL_H, DS_DARK_CURSOR);
    }

    /* 状态栏 */
    int status_y = cy + ch - 20;
    du_fill_rect(&g_fb, cx, status_y, cw, 20, DP_ABYSS_800);
    char status[64];
    int sp = 0;
    /* 行:列 */
    const char *p1 = "L:"; while(*p1) status[sp++] = *p1++;
    status[sp++] = '0' + (s->cursor_line+1)/10%10;
    status[sp++] = '0' + (s->cursor_line+1)%10;
    const char *p2 = " C:"; while(*p2) status[sp++] = *p2++;
    status[sp++] = '0' + (s->cursor_col+1)/10%10;
    status[sp++] = '0' + (s->cursor_col+1)%10;
    if (s->modified) { const char *m = " *"; while(*m) status[sp++] = *m++; }
    status[sp] = 0;
    du_draw_string(&g_fb, status, cx + 4, status_y + 1,
                   DS_DARK_TEXT_DIM, DP_ABYSS_800, DU_ASCII_STEP);
}

static void editor_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  内置应用：计算器 (Calculator)
 * ============================================================ */

typedef struct {
    char display[32];
    int  display_len;
    i64  accumulator;
    i64  current;
    int  op;          /* 0=none, 1=+, 2=-, 3=*, 4=/ */
    int  new_number;
} calc_state;

static void calc_update_display(calc_state *s) {
    s->display_len = 0;
    i64 val = s->current;
    if (val < 0) { s->display[s->display_len++] = '-'; val = -val; }
    if (val == 0) { s->display[s->display_len++] = '0'; }
    else {
        char tmp[20]; int tl = 0;
        while (val > 0) { tmp[tl++] = '0' + (int)(val % 10); val /= 10; }
        for (int i = tl-1; i >= 0; i--) s->display[s->display_len++] = tmp[i];
    }
    s->display[s->display_len] = 0;
}

static void *calc_on_create(app_ctx *ctx) {
    calc_state *s = (calc_state *)0x6000000;
    s->accumulator = 0; s->current = 0;
    s->op = 0; s->new_number = 1;
    calc_update_display(s);
    const char *t = "Calculator";
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];
    return s;
}

static void calc_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    calc_state *s = (calc_state *)state;
    if (ev->type != EV_KEY_DOWN) return;
    char c = scan_to_ascii(ev->scancode, ev->shift);
    if (c >= '0' && c <= '9') {
        if (s->new_number) { s->current = 0; s->new_number = 0; }
        s->current = s->current * 10 + (c - '0');
    } else if (c == '+') { s->accumulator = s->current; s->op = 1; s->new_number = 1; }
    else if (c == '-') { s->accumulator = s->current; s->op = 2; s->new_number = 1; }
    else if (c == '*') { s->accumulator = s->current; s->op = 3; s->new_number = 1; }
    else if (c == '/') { s->accumulator = s->current; s->op = 4; s->new_number = 1; }
    else if (c == '=' || c == '\n') {
        switch (s->op) {
        case 1: s->current = s->accumulator + s->current; break;
        case 2: s->current = s->accumulator - s->current; break;
        case 3: s->current = s->accumulator * s->current; break;
        case 4: s->current = (s->current != 0) ? s->accumulator / s->current : 0; break;
        }
        s->op = 0; s->new_number = 1;
    } else if (c == 'c' || c == 'C') {
        s->current = 0; s->accumulator = 0; s->op = 0; s->new_number = 1;
    }
    calc_update_display(s);
    ctx->win->dirty = 1;
}

static void calc_on_draw(void *state, app_ctx *ctx) {
    calc_state *s = (calc_state *)state;
    int cx = ctx->client_x;
    int cy = ctx->client_y;
    int cw = ctx->client_w;
    int ch = ctx->client_h;

    du_fill_rect(&g_fb, cx, cy, cw, ch, DS_DARK_BG_SECONDARY);

    /* 显示屏 */
    int disp_h = 40;
    du_fill_rounded_rect(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                          DP_ABYSS_900, DU_RADIUS_MD);
    du_rect_outline(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                    DS_DARK_BORDER, DU_RADIUS_MD);
    /* 数字右对齐 */
    int text_x = cx + cw - 16 - s->display_len * (int)DU_ASCII_STEP - 8;
    du_draw_string(&g_fb, s->display, text_x, cy + 16,
                   DS_DARK_TEXT_PRIMARY, DP_ABYSS_900, DU_ASCII_STEP);

    /* 按钮网格 4×4 */
    const char *btn_labels[] = {
        "C", "+/-", "%", "/",
        "7", "8", "9", "*",
        "4", "5", "6", "-",
        "1", "2", "3", "+",
        "0", ".",  "=",  0
    };
    int btn_w = (cw - 16 - 12) / 4;
    int btn_h = 32;
    int btn_y = cy + 8 + disp_h + 8;
    for (int i = 0; i < 19; i++) {
        int row = i / 4, col = i % 4;
        int bx = cx + 8 + col * (btn_w + 3);
        int by = btn_y + row * (btn_h + 3);
        if (by + btn_h > cy + ch - 4) break;
        u32 bg = (i < 4) ? DP_ABYSS_600 :
                 (i % 4 == 3) ? DS_DARK_ACCENT :
                 (i == 18) ? DP_SUCCESS :
                 DP_ABYSS_700;
        du_fill_rounded_rect(&g_fb, bx, by, btn_w, btn_h, bg, DU_RADIUS_SM);
        du_draw_string(&g_fb, btn_labels[i],
                       bx + (btn_w - 1*(int)DU_ASCII_STEP)/2,
                       by + (btn_h - (int)DU_ASCII_CELL_H)/2,
                       DS_DARK_TEXT_PRIMARY, bg, DU_ASCII_STEP);
    }
}

static void calc_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  应用注册
 * ============================================================ */

static void register_apps(void) {
    /* 0: Terminal */
    g_apps[0] = (app_descriptor){
        "bash", "Terminal", 640, 440,
        bash_on_create, bash_on_event, bash_on_draw, bash_on_destroy
    };
    g_app_count++;

    /* 1: Editor */
    g_apps[1] = (app_descriptor){
        "editor", "Editor", 600, 450,
        editor_on_create, editor_on_event, editor_on_draw, editor_on_destroy
    };
    g_app_count++;

    /* 2: File Manager */
    g_apps[2] = (app_descriptor){
        "fileman", "Files", 500, 400,
        0, 0, 0, 0 /* placeholder */
    };
    g_app_count++;

    /* 3: Calculator */
    g_apps[3] = (app_descriptor){
        "calc", "Calculator", 280, 400,
        calc_on_create, calc_on_event, calc_on_draw, calc_on_destroy
    };
    g_app_count++;
}

static void setup_desktop_icons(void) {
    int start_x = (int)g_fb_w - DESKTOP_ICON_W - ICON_GUTTER_X;
    int y = ICON_GUTTER_Y;
    for (int i = 0; i < g_app_count; i++) {
        if (!g_apps[i].on_create) continue;
        g_icons[g_icon_count].app_id = i;
        g_icons[g_icon_count].x = start_x;
        g_icons[g_icon_count].y = y;
        g_icons[g_icon_count].label = g_apps[i].display_name;
        g_icon_count++;
        y += DESKTOP_ICON_H;
    }
}

/* ============================================================
 *  应用启动
 * ============================================================ */

static void launch_app(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return;
    app_descriptor *app = &g_apps[app_id];
    if (!app->on_create) return;

    /* 计算窗口位置（居中偏移） */
    int wx = 80 + g_win_count * 30;
    int wy = 40 + g_win_count * 30;
    if (wx + (int)app->default_w > (int)g_fb_w - 20) wx = 80;
    if (wy + (int)app->default_h > (int)g_fb_h - TASKBAR_H - 20) wy = 40;

    desktop_window *win = win_create(app_id, app->display_name,
                                      wx, wy, (int)app->default_w, (int)app->default_h);
    if (!win) return;

    /* 构建应用上下文 */
    app_ctx ctx;
    ctx.fb = &g_fb;
    ctx.win = win;
    ctx.client_x = win->x + BORDER_W;
    ctx.client_y = win->y + TITLEBAR_H;
    ctx.client_w = win->w - 2 * BORDER_W;
    ctx.client_h = win->h - TITLEBAR_H - BORDER_W;
    ctx.should_exit = 0;
    ctx.block_read = g_block_read;
    ctx.kernel_api = g_kernel_api;

    win->app_state = app->on_create(&ctx);
    win->dirty = 1;

    /* Window open animation - expanding rectangle */
    for (int step = 0; step < 6; step++) {
        int scale = (step + 1) * 100 / 6;
        int aw = win->w * scale / 100;
        int ah = win->h * scale / 100;
        int ax = win->x + (win->w - aw) / 2;
        int ay = win->y + (win->h - ah) / 2;
        cursor_restore_bg();
        redraw_all();
        /* redraw_all 末尾已绘制光标，这里先擦除以保证保存的背景干净（不含光标像素） */
        cursor_restore_bg();
        du_rect_outline(&g_fb, ax, ay, aw, ah, DS_DARK_ACCENT, DU_RADIUS_SM);
        cursor_save_bg(g_mouse_x, g_mouse_y);
        cursor_draw(g_mouse_x, g_mouse_y);
        anim_delay_poll_mouse(16);
    }

    slog("app launched");
}

/* ============================================================
 *  全屏重绘
 * ============================================================ */

static void redraw_all(void) {
    /* 渐变背景 */
    du_fill_bg_gradient(&g_fb, DP_ABYSS_900, DP_ABYSS_800);

    /* 桌面图标 */
    draw_desktop_icons();

    /* 窗口（从底到顶） */
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (w->minimized || !w->visible) continue;

        /* 窗口装饰（先画框架作为窗口背景：主体、标题栏、边框） */
        draw_window_frame(w);

        /* 窗口客户区背景 */
        int cx = w->x + BORDER_W;
        int cy = w->y + TITLEBAR_H;
        int cw = w->w - 2 * BORDER_W;
        int ch = w->h - TITLEBAR_H - BORDER_W;

        /* 调用应用绘制（在框架之上绘制客户区内容） */
        if (w->app_id >= 0 && w->app_id < g_app_count && g_apps[w->app_id].on_draw && w->app_state) {
            app_ctx ctx;
            ctx.fb = &g_fb;
            ctx.win = w;
            ctx.client_x = cx; ctx.client_y = cy;
            ctx.client_w = cw; ctx.client_h = ch;
            ctx.should_exit = 0;
            ctx.block_read = g_block_read;
            ctx.kernel_api = g_kernel_api;
            g_apps[w->app_id].on_draw(w->app_state, &ctx);
        }
    }

    /* 任务栏 */
    draw_taskbar();

    /* 鼠标光标 */
    cursor_save_bg(g_mouse_x, g_mouse_y);
    cursor_draw(g_mouse_x, g_mouse_y);
}

/* ============================================================
 *  主循环
 * ============================================================ */

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    slog("boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        slog("bad context");
        for(;;) __asm__("hlt");
    }

    g_fb_addr = ctx->framebuffer_address;
    g_fb_w = ctx->framebuffer_width;
    g_fb_h = ctx->framebuffer_height;
    g_fb_pitch = ctx->framebuffer_pitch;
    g_kernel_api = (const void *)ctx->dkm_kernel_api;
    g_block_read = (desktop_block_read_fn)(*(u64 *)(ctx->dkm_kernel_api + 0xA8));

    du_context_init(&g_fb, g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch);

    /* 初始化 PS/2 鼠标 */
    ps2_mouse_init();

    /* 注册应用 */
    register_apps();
    setup_desktop_icons();

    /* 初始绘制 */
    redraw_all();
    slog("desktop ready");

    /* 主事件循环 */
    for (;;) {
        int need_redraw = 0;
        g_mouse_has_pkt = 0;

        /* 轮询 PS/2 鼠标 */
        if (ps2_mouse_poll()) {
            /* 拖拽处理 - 需要全屏重绘 */
            if (g_dragging && g_drag_win >= 0) {
                desktop_window *w = win_find(g_drag_win);
                if (w) {
                    w->x = g_mouse_x - g_drag_off_x;
                    w->y = g_mouse_y - g_drag_off_y;
                }
                need_redraw = 1;
            }

            /* 鼠标点击事件 - 需要全屏重绘 */
            if (g_mouse_btn) {
                /* 检查任务栏点击 */
                int tb_y = (int)g_fb_h - TASKBAR_H;
                if (g_mouse_y >= tb_y) {
                    /* 快速启动图标 */
                    int qx = 64;
                    for (int i = 0; i < g_app_count && i < 4; i++) {
                        int icon_x = qx + i * 40;
                        if (g_mouse_x >= icon_x && g_mouse_x < icon_x + 40) {
                            launch_app(i);
                            need_redraw = 1;
                            break;
                        }
                    }
                } else {
                    /* 窗口命中测试 */
                    desktop_window *hit = win_find_at(g_mouse_x, g_mouse_y);
                    if (hit) {
                        win_bring_to_front(hit);
                        /* 取消其他焦点 */
                        for (int i = 0; i < g_win_count; i++)
                            g_windows[i].focused = (g_windows[i].id == hit->id);
                        g_focused_win = hit->id;

                        /* 检查关闭按钮 */
                        if (win_hit_close_btn(hit, g_mouse_x, g_mouse_y)) {
                            win_destroy(hit);
                            need_redraw = 1;
                        }
                        /* 检查标题栏拖拽 */
                        else if (win_hit_titlebar(hit, g_mouse_x, g_mouse_y)) {
                            g_dragging = 1;
                            g_drag_win = hit->id;
                            g_drag_off_x = g_mouse_x - hit->x;
                            g_drag_off_y = g_mouse_y - hit->y;
                        }
                        /* 传递鼠标事件给应用 */
                        else if (hit->app_id >= 0 && g_apps[hit->app_id].on_event && hit->app_state) {
                            desktop_event dev;
                            dev.type = EV_MOUSE_DOWN;
                            dev.mx = g_mouse_x; dev.my = g_mouse_y;
                            dev.button = 0;
                            app_ctx actx;
                            actx.fb = &g_fb; actx.win = hit;
                            actx.client_x = hit->x + BORDER_W;
                            actx.client_y = hit->y + TITLEBAR_H;
                            actx.client_w = hit->w - 2*BORDER_W;
                            actx.client_h = hit->h - TITLEBAR_H - BORDER_W;
                            actx.should_exit = 0;
                            actx.block_read = g_block_read;
                            actx.kernel_api = g_kernel_api;
                            g_apps[hit->app_id].on_event(hit->app_state, &actx, &dev);
                        }
                    } else {
                        /* 桌面图标点击 */
                        for (int i = 0; i < g_icon_count; i++) {
                            int ix = g_icons[i].x, iy = g_icons[i].y;
                            if (g_mouse_x >= ix && g_mouse_x < ix + 32 &&
                                g_mouse_y >= iy && g_mouse_y < iy + 32) {
                                launch_app(g_icons[i].app_id);
                                need_redraw = 1;
                                break;
                            }
                        }
                    }
                }
            } else {
                if (g_dragging) {
                    g_dragging = 0;
                    g_drag_win = -1;
                }
            }
        }

        /* 轮询 PS/2 键盘 */
        {
            u8 st = inb(0x64);
            if ((st & 1) && !(st & 0x20)) {
                u8 data = inb(0x60);
                u8 sc = data;
                if (sc == 0xE0) { g_e0 = 1; goto next; }
                if (sc == 0x2A || sc == 0x36) { g_shift = 1; goto next; }
                if (sc == 0xAA || sc == 0xB6) { g_shift = 0; goto next; }
                if (sc & 0x80) { g_e0 = 0; goto next; }

                /* 发送键盘事件到焦点窗口 */
                if (g_focused_win >= 0) {
                    desktop_window *fw = win_find(g_focused_win);
                    if (fw && fw->app_id >= 0 && g_apps[fw->app_id].on_event && fw->app_state) {
                        desktop_event dev;
                        dev.type = EV_KEY_DOWN;
                        dev.mx = g_mouse_x; dev.my = g_mouse_y;
                        dev.button = 0;
                        dev.scancode = sc;
                        dev.shift = g_shift;
                        dev.ctrl = g_ctrl;
                        dev.alt = g_alt;
                        app_ctx actx;
                        actx.fb = &g_fb; actx.win = fw;
                        actx.client_x = fw->x + BORDER_W;
                        actx.client_y = fw->y + TITLEBAR_H;
                        actx.client_w = fw->w - 2*BORDER_W;
                        actx.client_h = fw->h - TITLEBAR_H - BORDER_W;
                        actx.should_exit = 0;
                        actx.block_read = g_block_read;
                        actx.kernel_api = g_kernel_api;
                        g_apps[fw->app_id].on_event(fw->app_state, &actx, &dev);
                        need_redraw = 1;
                    }
                }
                g_e0 = 0;
            }
        }
next:

        if (need_redraw) {
            cursor_restore_bg();
            redraw_all();
        } else if (g_mouse_has_pkt) {
            /* 鼠标移动但无需全屏重绘：只更新光标位置 */
            cursor_restore_bg();
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
        }

        __asm__("pause");
    }
}
