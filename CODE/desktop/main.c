/* Deshab Desktop Manager — Winux-Kate 结构完整复刻
 *
 * 布局结构与 D:\Code\Winux-Kate 完全一致：
 *   app-shell
 *   ├── topbar (34px)：品牌 + 工作区切换 + 页面切换 + PG 时钟
 *   page-host
 *   │   ├── 1·DASHBOARD：TERM-01 / TERM-02 / EDITOR / FILES 四面板 + StatusBar
 *   │   ├── 2·IDE：标签栏 + 编辑宿主
 *   │   ├── 3·DESKTOP：图标网格 + 窗口 + 任务栏
 *   │   └── 4+/CUSTOM：外部 .elf 全屏宿主（双击启动）
 *   └── task-view overlay（工作区卡片）
 *
 * 新 UI 特性（Kate 之外）：
 *   - RTC 实时时钟（秒级）+ 开机至今 UP 计时
 *   - VOL / BRIGHT 可拖滑块（BRIGHT 真实调暗全屏）
 *   - 开机打字机 Boot 屏
 *   - FILES 面板双击文件载入 EDITOR（FAT32 真实读写 + SAVE 回写）
 *   - 窗口单实例聚焦策略
 */

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/linux_compat.h"   /* VSCode Phase 2: input_forward_* */
#include "../UTSM/include/utsm/pe.h"            /* P5d: PE 窗口模式 run_windowed/inject_* */

/* block 设备函数类型（从 kernel_api + 0xA8 获取 block_api，read @ +0x10, write @ +0x18） */
typedef int (*desktop_block_read_fn)(u32 index, u64 lba, u32 count, void *buffer);
typedef int (*desktop_block_write_fn)(u32 index, u64 lba, u32 count, const void *buffer);

/* 先包含 ascii_bitmaps.c（定义 g_ascii），再包含 deshab_ui.h（引用 g_ascii） */
#include "../firstInit/ascii_bitmaps.c"
#include "../UTSM/include/utsm/deshab_ui.h"
/* 用户态 FAT32 读写。P5d：PE exe 可达数 MB，数据缓冲同 cmd.elf 提到 4MB。 */
#define F32_DATA_BYTES (4u * 1024u * 1024u)
#include "../tools/fat32_io.h"

typedef unsigned char      u8;
typedef signed char        i8;
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

static int kstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void kstrcpy(char *d, const char *s, int cap) {
    int i = 0;
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void kstrcat(char *d, const char *s, int cap) {
    int n = kstrlen(d);
    if (n < cap) kstrcpy(d + n, s, cap - n);
}

/* P7.9: u32 -> decimal string（返回长度） */
static int u32dec(u32 v, char *out) {
    char tmp[10];
    int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 前向声明 */
static void redraw_all(void);
static int  ps2_mouse_poll(void);
static void draw_statusbar_full(void);
static void files_refresh(void);

/* ============================================================
 *  常量（Winux-Kate 布局）
 * ============================================================ */

#define MAX_WINDOWS        8
#define BORDER_W           1
#define CURSOR_SIZE        24

#define ICON_W             KATE_ICON_W     /* 104 */
#define ICON_H             KATE_ICON_H     /* 90 */
#define ICON_GAP           KATE_ICON_GAP   /* 18 */
#define ICON_PAD           KATE_ICON_PAD   /* 24 */

#define CLOSE_BTN_SIZE     16
#define CLOSE_BTN_OFFSET   22

#define MAX_WS             6    /* 工作区上限 */
#define MAX_CPAGES         4    /* 自定义页上限 */
#define MAX_FILES          24   /* FILES 面板条目上限 */

/* 应用固定状态地址（单实例策略 → 每类应用唯一实例） */
#define ADDR_TERM1   0x4000000ULL
#define ADDR_TERM2   0x4100000ULL
#define ADDR_TERMWIN 0x4200000ULL
#define ADDR_EDDASH  0x5000000ULL
#define ADDR_EDWIN   0x5100000ULL
#define ADDR_CALC    0x6000000ULL

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
    int mx, my;
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
    int    app_id;
    void  *app_state;
    int    closing;
    int    dirty;
    int    ws;              /* 所属工作区 */
} desktop_window;

typedef struct desktop_window desktop_win;

typedef struct app_ctx {
    du_context  *fb;
    desktop_win *win;
    int          client_x, client_y, client_w, client_h;
    int          should_exit;
    desktop_block_read_fn  block_read;
    desktop_block_write_fn block_write;
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
    const char *elf_name;
    /* VSCode Phase 5: Linux guest 应用（来自 LINUXAPP.CNF）。
     * linux_path 非 0 时 launch_app 走 launch_linux_app → exec_async。 */
    const char *linux_path;
    const char *linux_args;
    /* P5d: Windows PE 应用（来自 PEAPPS.CNF）。
     * pe_file 非 0 时 launch_app 走 launch_pe_app → run_windowed 桌面嵌入。 */
    const char *pe_file;
    const char *pe_args;
} app_descriptor;

/* 桌面图标 */
typedef struct {
    int    app_id;
    int    x, y;
    const char *label;
    int    selected;
} desktop_icon;

#define MAX_ICONS 16   /* P7.4: 与 MAX_APPS 对齐（多实例 VSCODE2 等占位） */

/* 工作区 */
typedef struct {
    char name[16];
    int  page;      /* 该工作区离开时的页面 */
} workspace;

/* 自定义页 */
typedef struct {
    char name[12];
    char elf11[11];
    int  id;        /* 页面号 5..8 */
} cpage;

/* FILES 面板条目 */
typedef struct {
    char name11[11];
    char disp[13];
    u32  size;
} fentry;

/* ============================================================
 *  全局状态
 * ============================================================ */

static du_context g_fb;
static u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch;
static const void *g_kernel_api;

/* 双缓冲：渲染到 sprite buffer，完成后一次性 flip 到真实 framebuffer */
#define SPRITE_BUF_ADDR 0x7000000ULL          /* 112MB，在 identity-mapped 区域 */
static u8 *g_real_fb = 0;                     /* 真实 framebuffer 地址 */
static desktop_block_read_fn  g_block_read;
static desktop_block_write_fn g_block_write;
static const dsk_boot_context *g_boot_ctx = 0;

/* VSCode Phase 2: Linux guest 输入转发。
 * g_lxc_svc 由 dsk_entry() 从 boot context reserved[5] 读取；
 * g_input_forward_enabled 在 IDE 标签 attach/detach 时联动置位，
 * 开启后 PS/2 鼠标键盘事件经 virtio-input 注入 Linux guest（VSCode 运行其中）。 */
static const linux_compat_service *g_lxc_svc = 0;
static int g_input_forward_enabled = 0;

/* P5d: PE 窗口桌面嵌入状态。
 * g_pe_svc 由 dsk_entry() 从 boot context reserved[4] 读取（magic 校验）。
 * launch_pe_app 创建受管窗口后阻塞在 pe_service.run_windowed；
 * PE 消息空转时 shim 回调 pe_pump_cb 驱动 desktop 一帧。
 * 单实例：run_windowed 阻塞期间拒绝启动第二个 PE 窗口（pump 重入安全）。 */
static const pe_service *g_pe_svc = 0;
static int   g_pe_running = 0;
static int   g_pe_win_id = -1;
static u8   *g_pe_surf = 0;
static int   g_pe_surf_w = 0, g_pe_surf_h = 0;
static int   g_pe_state_dummy = 0;     /* draw_desktop_page 要求 app_state 非空 */

#define PE_SURFACE_ADDR  0x7800000ULL  /* 120MB：sprite buf(112MB, ≤8.3MB) 之上 */
#define PE_SURFACE_MAX   (8u * 1024u * 1024u)
#define PE_WM_CLOSE      0x0010u       /* shim Win32 WM_CLOSE */

/* 鼠标状态 */
static int g_mouse_x = 400, g_mouse_y = 300;
static int g_mouse_btn = 0;      /* bit0=左 bit1=右 */
static int g_left_pressed = 0, g_left_released = 0, g_right_pressed = 0;
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
#define MAX_APPS 16
static app_descriptor g_apps[MAX_APPS];
static int g_app_count = 0;

static desktop_icon g_icons[MAX_ICONS];
static int g_icon_count = 0;

/* ---- Kate 外壳状态 ---- */
static int g_booted = 0;
static int g_page = 1;                 /* 1..3 内置, 4..8 自定义 */
static int g_taskview = 0;
static int g_dash_focus = 0;           /* 0=t1 1=t2 2=ed 3=files */

static workspace g_ws[MAX_WS];
static int g_ws_count = 1;
static int g_ws_cur = 0;

static cpage g_cpages[MAX_CPAGES];
static int g_cpage_count = 0;

/* 右键菜单（移到工作区） */
static int g_ctx_open = 0;
static int g_ctx_x = 0, g_ctx_y = 0;
static int g_ctx_win = -1;

/* 状态栏滑块 */
static int g_vol = 65;
static int g_bright = 100;
static int g_bright_applied = 100;  /* 上次应用的亮度值（避免重复处理） */
static int g_drag_slider = 0;          /* 1=vol 2=bright */
static int g_vol_rect[4];              /* x,y,w,h 滑轨 */
static int g_bri_rect[4];

static int g_quit = 0;                 /* EXIT 按钮 → 返回 DSK */

/* 时钟 */
static int g_rtc_boot_sec = 0;         /* 开机时刻（秒级当天秒） */
static int g_last_sec = -1;
static u64 g_tsc_per_sec = 0;

/* FILES 面板 */
static fentry g_files[MAX_FILES];
static int g_file_count = 0;
static int g_file_sel = -1;
static int g_files_loaded = 0;

/* 双击检测 */
static u64 g_last_click_tsc = 0;
static int g_last_click_x = -100, g_last_click_y = -100;

/* 面板/控件命中矩形（绘制时更新，点击时使用） */
static int g_dash_rect[4][4];          /* 4 面板 x,y,w,h */
static int g_save_rect[4];
static int g_pg_rect[8][4];            /* 页面切换按钮 */
static int g_pg_rm_rect[8][4];
static int g_pgadd_rect[4];
static int g_tvbtn_rect[4];            /* 任务视图按钮 */
static int g_ws_rect[MAX_WS][4];
static int g_wsadd_rect[4];
static int g_tb_rect[MAX_WINDOWS][4];  /* 任务栏项 */
static int g_tb_close_rect[MAX_WINDOWS][4];
static int g_tb_quit_rect[4];
static int g_tb_adopt_rect[4];           /* 收纳窗口按钮 */
static int g_tv_card[MAX_WS + 1][4];   /* 任务视图卡片(+add) */
static int g_tv_close[MAX_WS][4];
static int g_ctx_rect[4];
static int g_ctx_item[MAX_WS + 1][4];
static int g_ctx_item_ws[MAX_WS + 1];   /* 菜单行 → 真实工作区索引，-1=新建 */
static int g_ide_host[4];
static int g_ide_tabx[4];              /* IDE 标签 × */
static int g_ide_new_rect[4];          /* IDE "+ NEW IDE" 按钮 */
static int g_cp_host[4];               /* 自定义页宿主 */

/* ============================================================
 *  脏矩形渲染（性能优化：只更新变化区域）
 * ============================================================ */

#define MAX_DIRTY_RECTS  16

typedef struct {
    int x, y, w, h;
    int active;
} dirty_rect;

static dirty_rect g_dirty_rects[MAX_DIRTY_RECTS];
static int g_dirty_count = 0;
static int g_full_redraw = 1;  /* 初始全屏重绘 */

/* 标记脏区域 */
__attribute__((unused))
static void mark_dirty(int x, int y, int w, int h) {
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
static void merge_dirty_rects(int *out_x, int *out_y, int *out_w, int *out_h) {
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
static void clear_dirty_rects(void) {
    g_dirty_count = 0;
    g_full_redraw = 0;
}

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
 *  RTC 时钟
 * ============================================================ */

static int bcd2bin(u8 v) { return (v >> 4) * 10 + (v & 0xF); }

static void rtc_read(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss) {
    outb(0x70, 0x00); *ss = bcd2bin(inb(0x71));
    outb(0x70, 0x02); *mi = bcd2bin(inb(0x71));
    outb(0x70, 0x04); *hh = bcd2bin(inb(0x71));
    outb(0x70, 0x07); *dd = bcd2bin(inb(0x71));
    outb(0x70, 0x08); *mo = bcd2bin(inb(0x71));
    outb(0x70, 0x09); *yy = bcd2bin(inb(0x71));
}

static int rtc_day_sec(void) {
    int y, mo, d, h, mi, s;
    rtc_read(&y, &mo, &d, &h, &mi, &s);
    return h * 3600 + mi * 60 + s;
}

/* 格式化 HH:MM:SS */
static void fmt_hms(char *out, int sec) {
    int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
    out[0] = '0' + h / 10; out[1] = '0' + h % 10; out[2] = ':';
    out[3] = '0' + m / 10; out[4] = '0' + m % 10; out[5] = ':';
    out[6] = '0' + s / 10; out[7] = '0' + s % 10; out[8] = 0;
}

/* 校准 TSC（约 1 秒）：用于双击间隔判定 */
static void tsc_calibrate(void) {
    int s0 = rtc_day_sec();
    while (rtc_day_sec() == s0) __asm__("pause");
    u64 t0 = rdtsc();
    int s1 = rtc_day_sec();
    while (rtc_day_sec() == s1) __asm__("pause");
    g_tsc_per_sec = rdtsc() - t0;
}

/* ============================================================
 *  Winux-Kate 应用图标（40×40，青色几何图形）
 * ============================================================ */

static void draw_circle_outline(du_context *ctx, int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 > (r - 1) * (r - 1)) {
                du_pixel(ctx, cx + dx, cy + dy, color);
            }
        }
    }
}

static void draw_icon_terminal(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    du_fill_rect(ctx, cx + 8,  cy + 12, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 14, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 12, cy + 16, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 18, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 8,  cy + 20, 2, 2, KS_ACCENT2);
    du_fill_rect(ctx, cx + 16, cy + 22, 8, 2, KS_ACCENT);
}

static void draw_icon_editor(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    int widths[] = {24, 20, 22, 14, 20, 18};
    for (int i = 0; i < 6; i++) {
        du_fill_rect(ctx, cx + 8, cy + 8 + i * 4, widths[i], 2,
                     (i == 0) ? KS_ACCENT2 : KS_ACCENT);
    }
}

static void draw_icon_folder(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_BORDER, 2);
    du_fill_rect(ctx, cx + 8, cy + 10, 12, 4, KS_ACCENT_DIM);
    du_rect_outline(ctx, cx + 8, cy + 10, 12, 4, KS_ACCENT, 1);
    du_fill_rect(ctx, cx + 6, cy + 14, 28, 16, KS_BG_SECONDARY);
    du_rect_outline(ctx, cx + 6, cy + 14, 28, 16, KS_ACCENT, 1);
    du_fill_rect(ctx, cx + 6, cy + 14, 28, 1, KS_ACCENT);
    du_fill_rect(ctx, cx + 10, cy + 19, 12, 1, KS_ACCENT2);
    du_fill_rect(ctx, cx + 10, cy + 23, 16, 1, KS_TEXT_DIM);
    du_fill_rect(ctx, cx + 10, cy + 26, 10, 1, KS_TEXT_DIM);
}

static void draw_icon_calc(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    du_fill_rect(ctx, cx + 6, cy + 6, 28, 8, KS_BG_PRIMARY);
    du_rect_outline(ctx, cx + 6, cy + 6, 28, 8, KS_ACCENT2, 1);
    du_fill_rect(ctx, cx + 28, cy + 8, 4, 4, KS_ACCENT2);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = cx + 6 + c * 9;
            int by = cy + 18 + r * 7;
            du_fill_rect(ctx, bx, by, 7, 5, KS_ACCENT_DIM);
            du_rect_outline(ctx, bx, by, 7, 5, KS_ACCENT, 1);
        }
    }
}

static void draw_icon_browser(du_context *ctx, int cx, int cy) {
    du_fill_rect(ctx, cx, cy, 40, 40, KS_BG_TERTIARY);
    du_rect_outline(ctx, cx, cy, 40, 40, KS_ACCENT, 2);
    int ccx = cx + 20, ccy = cy + 20, r = 11;
    draw_circle_outline(ctx, ccx, ccy, r, KS_ACCENT);
    du_fill_rect(ctx, ccx - r, ccy, 2 * r + 1, 1, KS_ACCENT);
    du_fill_rect(ctx, ccx, ccy - r, 1, 2 * r + 1, KS_ACCENT);
    du_pixel(ctx, ccx, ccy, KS_ACCENT2);
}

static void draw_app_icon(du_context *ctx, int app_id, int cx, int cy) {
    switch (app_id) {
    case 0: draw_icon_terminal(ctx, cx, cy); break;
    case 1: draw_icon_editor(ctx, cx, cy);   break;
    case 2: draw_icon_folder(ctx, cx, cy);   break;
    case 3: draw_icon_calc(ctx, cx, cy);     break;
    case 4: draw_icon_browser(ctx, cx, cy);  break;
    default: draw_icon_terminal(ctx, cx, cy); break;
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
                du_pixel(&g_fb, mx + c, my + r, KS_ACCENT);
            }
        }
    }
}

/* ---- 双缓冲 flip ---- */

/* 将 sprite buffer 完整复制到真实 framebuffer */
static void flip_buffer(void) {
    if (!g_real_fb) return;
    u64 total = g_fb_h * g_fb_pitch / 4;
    u32 *dst = (u32 *)g_real_fb;
    u32 *src = (u32 *)SPRITE_BUF_ADDR;
    for (u64 i = 0; i < total; i++) dst[i] = src[i];
}

/* 仅复制指定矩形区域（用于鼠标移动时的局部更新） */
static void flip_rect(int x, int y, int w, int h) {
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
static void anim_delay_poll_mouse(u32 ms) {
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
 *  窗口管理
 * ============================================================ */

static desktop_window *win_find(int id) {
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].id == id) return &g_windows[i];
    }
    return 0;
}

static desktop_window *win_find_at(int mx, int my) {
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

static void win_focus(desktop_window *w) {
    int id = w->id;   /* bring_to_front 会重排数组，先保存 id 避免悬空指针 */
    win_bring_to_front(w);
    for (int i = 0; i < g_win_count; i++)
        g_windows[i].focused = (g_windows[i].id == id);
    g_focused_win = id;
    if (g_win_count > 0) g_windows[g_win_count - 1].minimized = 0;
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
    win->ws = g_ws_cur;
    for (int i = 0; i < 47 && title[i]; i++) win->title[i] = title[i];
    win->title[47] = 0;
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

static int win_hit_titlebar(desktop_window *w, int mx, int my) {
    return mx >= w->x && mx < w->x + w->w &&
           my >= w->y && my < w->y + KATE_TITLEBAR_H;
}

/* P5d: 关闭请求。运行中的 PE 窗口不能直接 win_destroy（run_windowed 阻塞中，
 * pump 持有窗口 id）——按 pe_window_host 协作契约 inject WM_CLOSE，
 * PE 自愿退出后由 launch_pe_app 收尾销毁。 */
static void win_close_request(desktop_window *w) {
    if (g_pe_running && w->id == g_pe_win_id && g_pe_svc && g_pe_svc->inject_input) {
        g_pe_svc->inject_input(PE_WM_CLOSE, 0, 0, 0, 0);
        return;
    }
    win_destroy(w);
}

static int win_hit_close_btn(desktop_window *w, int mx, int my) {
    int bx = w->x + w->w - CLOSE_BTN_OFFSET;
    int by = w->y + (KATE_TITLEBAR_H - CLOSE_BTN_SIZE) / 2;
    return mx >= bx && mx < bx + CLOSE_BTN_SIZE &&
           my >= by && my < by + CLOSE_BTN_SIZE;
}

static int ws_win_count(int ws) {
    int n = 0;
    for (int i = 0; i < g_win_count; i++)
        if (g_windows[i].ws == ws && g_windows[i].visible) n++;
    return n;
}

/* ============================================================
 *  窗口装饰绘制（Winux-Kate 风格）
 * ============================================================ */

static void draw_window_frame(desktop_window *w) {
    if (!w->visible || w->minimized) return;
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

/* ============================================================
 *  PS/2 鼠标处理
 * ============================================================ */

static int ps2_wait_write(void) {
    for (int t = 0; t < 200000; t++) {
        if (!(inb(0x64) & 0x02)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int ps2_wait_read(void) {
    for (int t = 0; t < 200000; t++) {
        if (inb(0x64) & 0x01) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static void ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(0x64) & 0x01)) break;
        inb(0x60);
    }
}

static void ps2_mouse_init(void) {
    ps2_drain();

    ps2_wait_write();
    outb(0x64, 0xA8);

    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    u8 cfg = inb(0x60);

    cfg &= ~0x20;
    cfg |= 0x02;
    cfg |= 0x40;

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, cfg);

    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xFF);
    for (int i = 0; i < 8; i++) {
        if (ps2_wait_read() != 0) break;
        inb(0x60);
    }

    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF4);
    for (int i = 0; i < 4; i++) {
        if (ps2_wait_read() != 0) break;
        inb(0x60);
    }

    ps2_drain();

    g_mouse_idx = 0;
    g_mouse_has_pkt = 0;

    slog("mouse init ok");
}

static int ps2_mouse_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    if (!(st & 0x20)) return 0;
    u8 data = inb(0x60);

    g_mouse_buf[g_mouse_idx++] = data;
    if (g_mouse_idx < 3) return 0;
    g_mouse_idx = 0;

    if (!(g_mouse_buf[0] & 0x08)) return 0;

    int dx = (int)(i8)g_mouse_buf[1];
    int dy = (int)(i8)g_mouse_buf[2];
    dy = -dy;

    g_mouse_x += dx;
    g_mouse_y += dy;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;
    if (g_mouse_x >= (int)g_fb_w - CURSOR_SIZE) g_mouse_x = (int)g_fb_w - CURSOR_SIZE;
    if (g_mouse_y >= (int)g_fb_h - CURSOR_SIZE) g_mouse_y = (int)g_fb_h - CURSOR_SIZE;

    int old_btn = g_mouse_btn;
    g_mouse_btn = g_mouse_buf[0] & 0x03;
    if ((g_mouse_btn & 1) && !(old_btn & 1)) g_left_pressed = 1;
    if (!(g_mouse_btn & 1) && (old_btn & 1)) g_left_released = 1;
    if ((g_mouse_btn & 2) && !(old_btn & 2)) g_right_pressed = 1;
    if (dx == 0 && dy == 0 && g_mouse_btn == old_btn) return 0;

    /* VSCode Phase 2: IDE attached 时把相对位移 + 按钮状态镜像到 Linux guest。
     * dy 已在上方取反为屏幕坐标系（正=下），与 Linux REL_Y 约定一致；
     * input_forward_mouse 内部与上次状态比较，仅产生变化按钮事件 + SYN。
     * 即使宿主光标已抵边界被钳位，原始 dx/dy 仍转发，guest 光标自由移动。 */
    if (g_input_forward_enabled && g_lxc_svc && g_lxc_svc->input_forward_mouse) {
        g_lxc_svc->input_forward_mouse(dx, dy, (u8)g_mouse_btn);
    }

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

/* VSCode Phase 2: PS/2 set-1 扫描码 → Linux keycode（linux/input-event-codes.h）。
 *   base : 已去除 0x80 release 位的基础扫描码（0x01..0x39 主键区 + 少数扩展）
 *   e0   : 上一字节为 0xE0 前缀时置 1，选取扩展键变体
 * 返回 0 表示该扫描码无映射（调用方应丢弃）。
 *
 * 巧合：主键区 0x01..0x39 的 Linux keycode 与扫描码数值完全一致
 * （IBM XT 布局历史遗留），故直接透传；仅 E0 扩展键需单独查表。 */
static u16 scancode_to_linux_keycode(u8 base, int e0) {
    if (e0) {
        switch (base) {
        case 0x1D: return 97;   /* KEY_RIGHTCTRL */
        case 0x38: return 100; /* KEY_RIGHTALT */
        case 0x48: return 103; /* KEY_UP */
        case 0x4B: return 105; /* KEY_LEFT */
        case 0x4D: return 106; /* KEY_RIGHT */
        case 0x50: return 108; /* KEY_DOWN */
        case 0x52: return 110; /* KEY_INSERT */
        case 0x53: return 111; /* KEY_DELETE */
        case 0x47: return 102; /* KEY_HOME */
        case 0x4F: return 107; /* KEY_END */
        case 0x49: return 104; /* KEY_PAGEUP */
        case 0x51: return 109; /* KEY_PAGEDOWN */
        default:   return 0;
        }
    }
    /* 主键区：0x01..0x39 透传，超出范围无映射 */
    if (base >= 0x01 && base <= 0x39) return (u16)base;
    return 0;
}

/* ============================================================
 *  终端 (Bash) — 面板/窗口共用核心
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
    /* 性能优化：脏行标记（增量渲染） */
    u8 dirty_rows[TERM_MAX_ROWS];  /* 标记哪些行需要重绘 */
    int any_dirty;                   /* 是否有任何脏行 */
} bash_state;

static const char *BASH_PROMPT = "deshab# ";

static void bash_reset(bash_state *s, int w, int h) {
    for (int i = 0; i < TERM_MAX_CHARS; i++) {
        s->ch[i] = ' '; s->fg[i] = KS_TEXT_PRIMARY;
    }
    s->cols = (w - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_STEP;
    s->rows = (h - (int)DU_SPACE_SM * 2) / (int)DU_ASCII_LINE_H;
    if (s->cols > TERM_MAX_COLS) s->cols = TERM_MAX_COLS;
    if (s->rows > TERM_MAX_ROWS) s->rows = TERM_MAX_ROWS;
    if (s->cols < 10) s->cols = 10;
    if (s->rows < 3) s->rows = 3;
    s->cur_col = 0; s->cur_row = 0;
    s->input_len = 0; s->input_cursor = 0;
    s->prompt_len = kstrlen(BASH_PROMPT);
    /* 初始化脏行标记：所有行标记为脏，确保首次渲染 */
    for (int i = 0; i < TERM_MAX_ROWS; i++) s->dirty_rows[i] = 1;
    s->any_dirty = 1;
}

static void bash_putc(bash_state *s, char c, u32 color) {
    if (c == '\n') {
        s->dirty_rows[s->cur_row] = 1;  /* 标记当前行为脏 */
        s->any_dirty = 1;
        s->cur_col = 0; s->cur_row++;
        return;
    }
    if (s->cur_row >= s->rows) {
        /* 滚动：所有行都需要重绘 */
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
        /* 标记所有行为脏 */
        for (int r = 0; r < s->rows; r++) s->dirty_rows[r] = 1;
        s->any_dirty = 1;
    }
    if (s->cur_col >= s->cols) { s->cur_col = 0; s->cur_row++; }
    int idx = s->cur_row * TERM_MAX_COLS + s->cur_col;
    s->ch[idx] = (u8)c; s->fg[idx] = color;
    s->cur_col++;
    /* 标记当前行为脏 */
    s->dirty_rows[s->cur_row] = 1;
    s->any_dirty = 1;
}

static void bash_puts(bash_state *s, const char *str, u32 color) {
    while (*str) bash_putc(s, *str++, color);
}

static void bash_execute(bash_state *s, const char *cmd) {
    while (*cmd == ' ') cmd++;
    bash_puts(s, BASH_PROMPT, KS_ACCENT2);
    bash_puts(s, cmd, KS_TEXT_PRIMARY);
    bash_putc(s, '\n', 0);

    if (*cmd == 0) return;
    if (cmd[0]=='h'&&cmd[1]=='e'&&cmd[2]=='l'&&cmd[3]=='p') {
        bash_puts(s, "  help    clear    echo    version\n", KS_ACCENT2);
        bash_puts(s, "  uname   about    reboot   halt\n", KS_ACCENT2);
        bash_puts(s, "  pwd     whoami   id\n", KS_ACCENT2);
    } else if (cmd[0]=='c'&&cmd[1]=='l'&&cmd[2]=='e'&&cmd[3]=='a'&&cmd[4]=='r') {
        for (int i = 0; i < TERM_MAX_CHARS; i++) { s->ch[i] = ' '; }
        s->cur_col = 0; s->cur_row = 0;
    } else if (cmd[0]=='v'&&cmd[1]=='e'&&cmd[2]=='r') {
        bash_puts(s, "Deshab OS v0.1.0 (Winux-Kate shell)\n", KS_ACCENT2);
    } else if (cmd[0]=='u'&&cmd[1]=='n'&&cmd[2]=='a') {
        bash_puts(s, "Deshab\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='a'&&cmd[1]=='b'&&cmd[2]=='o'&&cmd[3]=='u'&&cmd[4]=='t') {
        bash_puts(s, "Deshab OS - SAS-R0 Kernel\n", KS_ACCENT);
        bash_puts(s, "Winux-Kate UI Structure\n", KS_ACCENT2);
    } else if (cmd[0]=='e'&&cmd[1]=='c'&&cmd[2]=='h'&&cmd[3]=='o') {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        bash_puts(s, arg, KS_TEXT_PRIMARY);
        bash_putc(s, '\n', 0);
    } else if (cmd[0]=='p'&&cmd[1]=='w'&&cmd[2]=='d') {
        bash_puts(s, "/\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='w'&&cmd[1]=='h'&&cmd[2]=='o') {
        bash_puts(s, "root\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='i'&&cmd[1]=='d') {
        bash_puts(s, "uid=0(root) gid=0(root)\n", KS_TEXT_PRIMARY);
    } else if (cmd[0]=='r'&&cmd[1]=='e'&&cmd[2]=='b') {
        bash_puts(s, "Rebooting...\n", KS_ACCENT2);
        outb(0x64, 0xFE);
        for(;;) __asm__("hlt");
    } else if (cmd[0]=='h'&&cmd[1]=='a'&&cmd[2]=='l'&&cmd[3]=='t') {
        bash_puts(s, "Halted.\n", KS_ACCENT2);
        for(;;) __asm__("hlt");
    } else {
        bash_puts(s, "unknown: ", KS_DANGER);
        bash_puts(s, cmd, KS_DANGER);
        bash_putc(s, '\n', 0);
    }
}

static void bash_key(bash_state *s, u8 sc, int shift) {
    if (sc == 0x1C) {
        s->input_buf[s->input_len] = 0;
        bash_execute(s, s->input_buf);
        s->input_len = 0; s->input_cursor = 0;
    } else if (sc == 0x0E) {
        if (s->input_cursor > 0) {
            for (int i = s->input_cursor-1; i < s->input_len-1; i++)
                s->input_buf[i] = s->input_buf[i+1];
            s->input_len--; s->input_cursor--;
        }
    } else {
        char c = scan_to_ascii(sc, shift);
        if (c && c >= 32 && c <= 126 && s->input_len < 255) {
            for (int i = s->input_len; i > s->input_cursor; i--)
                s->input_buf[i] = s->input_buf[i-1];
            s->input_buf[s->input_cursor++] = c;
            s->input_len++;
        }
    }
}

/* 绘制终端到指定矩形区域（面板或窗口客户区）
 * 性能优化：增量渲染，只绘制脏行 */
static void bash_draw_to(bash_state *s, int cx, int cy, int cw, int ch) {
    /* 快速退出：终端内容未修改，跳过绘制 */
    if (!s->any_dirty) return;
    
    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_PRIMARY);

    for (int r = 0; r < s->rows; r++) {
        /* 增量渲染：只绘制脏行 */
        if (!s->dirty_rows[r]) continue;
        
        for (int c = 0; c < s->cols; c++) {
            int idx = r * TERM_MAX_COLS + c;
            if (s->ch[idx] == ' ') continue;
            int px = cx + (int)DU_SPACE_SM + c * (int)DU_ASCII_STEP;
            int py = cy + (int)DU_SPACE_SM + r * (int)DU_ASCII_LINE_H;
            if (px + (int)DU_ASCII_CELL_W > cx + cw) continue;
            if (py + (int)DU_ASCII_CELL_H > cy + ch) continue;
            du_draw_char(&g_fb, s->ch[idx], px, py, s->fg[idx], KS_BG_PRIMARY);
        }
        
        /* 清除脏行标记 */
        s->dirty_rows[r] = 0;
    }

    int input_y = cy + (int)DU_SPACE_SM + s->cur_row * (int)DU_ASCII_LINE_H;
    if (input_y + (int)DU_ASCII_CELL_H <= cy + ch) {
        du_draw_string(&g_fb, BASH_PROMPT,
                       cx + (int)DU_SPACE_SM, input_y,
                       KS_ACCENT2, KS_BG_PRIMARY, DU_ASCII_STEP);
        du_draw_string(&g_fb, s->input_buf,
                       cx + (int)DU_SPACE_SM + s->prompt_len * (int)DU_ASCII_STEP,
                       input_y,
                       KS_TEXT_PRIMARY, KS_BG_PRIMARY, DU_ASCII_STEP);
        int cursor_x = cx + (int)DU_SPACE_SM + (s->prompt_len + s->input_cursor) * (int)DU_ASCII_STEP;
        du_fill_rect(&g_fb, cursor_x, input_y + (int)DU_ASCII_CELL_H - 3,
                     (int)DU_ASCII_CELL_W, 2, KS_ACCENT);
    }
    
    /* 清除全局脏标记 */
    s->any_dirty = 0;
}

/* ---- 窗口适配 ---- */
__attribute__((unused))
static void *bash_on_create(app_ctx *ctx) {
    bash_state *s = (bash_state *)ADDR_TERMWIN;
    bash_reset(s, ctx->client_w, ctx->client_h);
    bash_puts(s, "Deshab Bash v0.1\nType help for commands\n\n", KS_TEXT_PRIMARY);
    return s;
}
__attribute__((unused))
static void bash_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    if (ev->type != EV_KEY_DOWN) return;
    bash_key((bash_state *)state, ev->scancode, ev->shift);
    ctx->win->dirty = 1;
}
__attribute__((unused))
static void bash_on_draw(void *state, app_ctx *ctx) {
    bash_draw_to((bash_state *)state, ctx->client_x, ctx->client_y,
                 ctx->client_w, ctx->client_h);
}
__attribute__((unused))
static void bash_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  文本编辑器 (Editor) — 面板/IDE/窗口共用核心
 * ============================================================ */

#define EDITOR_BUF_SIZE 65536

typedef struct {
    char text[EDITOR_BUF_SIZE];
    int  text_len;
    int  cursor_pos;
    int  scroll_y;
    int  cursor_line;
    int  cursor_col;
    int  modified;
    int  has_file;          /* 关联了磁盘文件 */
    char name11[11];        /* FAT32 8.3 名 */
    char disp[13];          /* 显示名 */
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

static void editor_reset(editor_state *s, const char *sample) {
    s->text_len = 0; s->cursor_pos = 0;
    s->scroll_y = 0; s->modified = 0;
    s->has_file = 0;
    s->disp[0] = 0;
    while (*sample && s->text_len < EDITOR_BUF_SIZE - 1) {
        s->text[s->text_len++] = *sample++;
    }
    s->cursor_pos = s->text_len;
    editor_recalc_cursor(s);
}

static void editor_key(editor_state *s, u8 sc, int shift) {
    if (sc == 0x1C) {
        if (s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = '\n';
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    } else if (sc == 0x0E) {
        if (s->cursor_pos > 0) {
            for (int i = s->cursor_pos - 1; i < s->text_len - 1; i++)
                s->text[i] = s->text[i+1];
            s->text_len--; s->cursor_pos--;
            s->modified = 1;
        }
    } else if (sc == 0x48) {
        if (s->cursor_line > 0) {
            int prev_start = editor_line_start(s, s->cursor_line - 1);
            int col = s->cursor_col;
            s->cursor_pos = prev_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x50) {
        int next_start = editor_line_start(s, s->cursor_line + 1);
        if (next_start < s->text_len || s->cursor_line < 999) {
            int col = s->cursor_col;
            s->cursor_pos = next_start;
            for (int i = 0; i < col && s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n'; i++)
                s->cursor_pos++;
        }
    } else if (sc == 0x4B) {
        if (s->cursor_pos > 0) s->cursor_pos--;
    } else if (sc == 0x4D) {
        if (s->cursor_pos < s->text_len) s->cursor_pos++;
    } else if (sc == 0x47) {
        s->cursor_pos = editor_line_start(s, s->cursor_line);
    } else if (sc == 0x4F) {
        s->cursor_pos = editor_line_start(s, s->cursor_line);
        while (s->cursor_pos < s->text_len && s->text[s->cursor_pos] != '\n')
            s->cursor_pos++;
    } else {
        char c = scan_to_ascii(sc, shift);
        if (c && c >= 32 && c <= 126 && s->text_len < EDITOR_BUF_SIZE - 1) {
            for (int i = s->text_len; i > s->cursor_pos; i--)
                s->text[i] = s->text[i-1];
            s->text[s->cursor_pos] = c;
            s->text_len++; s->cursor_pos++;
            s->modified = 1;
        }
    }
    editor_recalc_cursor(s);
}

/* 绘制编辑器到指定矩形区域 */
static void editor_draw_to(editor_state *s, int cx, int cy, int cw, int ch) {
    int line_num_w = 5 * (int)DU_ASCII_STEP + (int)DU_SPACE_SM;
    int text_area_x = cx + line_num_w;
    int text_area_w = cw - line_num_w;
    int visible_rows = (ch - 20) / (int)DU_ASCII_LINE_H;
    int visible_cols = text_area_w / (int)DU_ASCII_STEP;
    if (visible_rows < 1) visible_rows = 1;
    if (visible_cols < 4) visible_cols = 4;

    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_SECONDARY);

    if (s->cursor_line < s->scroll_y) s->scroll_y = s->cursor_line;
    if (s->cursor_line >= s->scroll_y + visible_rows) s->scroll_y = s->cursor_line - visible_rows + 1;

    int line = 0;
    int pos = 0;
    for (int i = 0; i < s->text_len || line <= s->cursor_line; ) {
        if (line >= s->scroll_y + visible_rows) break;

        if (line >= s->scroll_y) {
            int screen_y = cy + (line - s->scroll_y) * (int)DU_ASCII_LINE_H;

            char ln[8];
            ln[0] = '0' + ((line+1)/100)%10;
            ln[1] = '0' + ((line+1)/10)%10;
            ln[2] = '0' + (line+1)%10;
            ln[3] = 0;
            du_draw_string(&g_fb, ln, cx + 2, screen_y,
                           KS_TEXT_DIM, KS_BG_SECONDARY, DU_ASCII_STEP);

            du_fill_rect(&g_fb, cx + line_num_w - 2, screen_y, 1,
                         (int)DU_ASCII_CELL_H, KS_BORDER_DIM);

            int col = 0;
            while (pos < s->text_len && s->text[pos] != '\n' && col < visible_cols) {
                du_draw_char(&g_fb, s->text[pos],
                             text_area_x + col * (int)DU_ASCII_STEP,
                             screen_y,
                             KS_TEXT_PRIMARY, KS_BG_SECONDARY);
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

    /* 光标（青色竖条） */
    if (s->cursor_line >= s->scroll_y && s->cursor_line < s->scroll_y + visible_rows) {
        int cursor_y = cy + (s->cursor_line - s->scroll_y) * (int)DU_ASCII_LINE_H;
        int cursor_x = text_area_x + s->cursor_col * (int)DU_ASCII_STEP;
        du_fill_rect(&g_fb, cursor_x, cursor_y, 2, (int)DU_ASCII_CELL_H, KS_ACCENT);
    }

    /* 底部迷你状态栏 */
    int status_y = cy + ch - 20;
    du_fill_rect(&g_fb, cx, status_y, cw, 20, KS_BG_TERTIARY);
    du_divider_h(&g_fb, cx, status_y, cw, KS_BORDER);
    char status[64];
    int sp = 0;
    const char *p1 = "L:"; while(*p1) status[sp++] = *p1++;
    status[sp++] = '0' + (s->cursor_line+1)/10%10;
    status[sp++] = '0' + (s->cursor_line+1)%10;
    const char *p2 = " C:"; while(*p2) status[sp++] = *p2++;
    status[sp++] = '0' + (s->cursor_col+1)/10%10;
    status[sp++] = '0' + (s->cursor_col+1)%10;
    if (s->modified) { const char *m = " *"; while(*m) status[sp++] = *m++; }
    if (s->has_file) {
        const char *m = "  "; while(*m) status[sp++] = *m++;
        for (int i = 0; s->disp[i] && sp < 60; i++) status[sp++] = s->disp[i];
    }
    status[sp] = 0;
    du_draw_string(&g_fb, status, cx + 4, status_y + 1,
                   KS_TEXT_DIM, KS_BG_TERTIARY, DU_ASCII_STEP);
}

/* ---- 窗口适配 ---- */
static void *editor_on_create(app_ctx *ctx) {
    editor_state *s = (editor_state *)ADDR_EDWIN;
    editor_reset(s, "# Welcome to Deshab Editor\n#\n\nStart typing here...\n");
    for (int i = 0; i < 47; i++) ctx->win->title[i] = 0;
    const char *t = "Editor - untitled";
    for (int i = 0; t[i] && i < 47; i++) ctx->win->title[i] = t[i];
    return s;
}
static void editor_on_event(void *state, app_ctx *ctx, desktop_event *ev) {
    if (ev->type != EV_KEY_DOWN) return;
    editor_key((editor_state *)state, ev->scancode, ev->shift);
    ctx->win->dirty = 1;
}
static void editor_on_draw(void *state, app_ctx *ctx) {
    editor_draw_to((editor_state *)state, ctx->client_x, ctx->client_y,
                   ctx->client_w, ctx->client_h);
}
static void editor_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  计算器 (Calculator)
 * ============================================================ */

typedef struct {
    char display[32];
    int  display_len;
    i64  accumulator;
    i64  current;
    int  op;
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
    calc_state *s = (calc_state *)ADDR_CALC;
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

    du_fill_rect(&g_fb, cx, cy, cw, ch, KS_BG_SECONDARY);

    int disp_h = 40;
    du_fill_rounded_rect(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                          KS_BG_PRIMARY, DU_RADIUS_MD);
    du_rect_outline(&g_fb, cx + 8, cy + 8, cw - 16, disp_h,
                    KS_ACCENT, DU_RADIUS_MD);
    int text_x = cx + cw - 16 - s->display_len * (int)DU_ASCII_STEP - 8;
    du_draw_string(&g_fb, s->display, text_x, cy + 16,
                   KS_ACCENT, KS_BG_PRIMARY, DU_ASCII_STEP);

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
        u32 bg = (i < 4) ? KS_ACCENT_DIM :
                 (i % 4 == 3) ? KS_ACCENT :
                 (i == 18) ? KS_ACCENT2 :
                 KS_BG_TERTIARY;
        u32 fg = ((i % 4 == 3) || (i == 18)) ? KS_TEXT_INVERT : KS_TEXT_PRIMARY;
        du_fill_rounded_rect(&g_fb, bx, by, btn_w, btn_h, bg, DU_RADIUS_SM);
        du_rect_outline(&g_fb, bx, by, btn_w, btn_h, KS_BORDER, DU_RADIUS_SM);
        du_draw_string(&g_fb, btn_labels[i],
                       bx + (btn_w - 1*(int)DU_ASCII_STEP)/2,
                       by + (btn_h - (int)DU_ASCII_CELL_H)/2,
                       fg, bg, DU_ASCII_STEP);
    }
}

static void calc_on_destroy(void *state) { (void)state; }

/* ============================================================
 *  应用注册
 * ============================================================ */

/* ---- VSCode Phase 5: LINUXAPP.CNF Linux 应用注册表 ----
 * FAT32 根目录 LINUXAPP.CNF（8.3: LINUXAPPCNF），每行一个 Linux guest 应用：
 *   NAME|DISPLAY|/guest/path|args（args 可含空格，可省略）
 * '#' 开头为注释。解析结果存静态池，app_descriptor.linux_path 指入。 */
#define LINUXAPP_MAX 4
static char g_lapp_name[LINUXAPP_MAX][12];
static char g_lapp_disp[LINUXAPP_MAX][16];
static char g_lapp_path[LINUXAPP_MAX][96];
static char g_lapp_args[LINUXAPP_MAX][96];
static int  g_vscode_app_id = -1;   /* 第一个名为 "vscode" 的注册项 */

/* 字段拷贝：src[0..n) → dst（NUL 结尾），返回拷贝长度 */
static int lapp_field_copy(char *dst, int dst_cap, const char *src, int n) {
    if (n >= dst_cap) n = dst_cap - 1;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
    return n;
}

static void register_linux_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83("LINUXAPP.CNF", n11) != 0) return;
    if (f32_read_root_file(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

    int li = 0;                 /* 已注册数量 */
    u32 pos = 0;
    while (pos < size && li < LINUXAPP_MAX && g_app_count < MAX_APPS) {
        /* 取一行 */
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 line_len = eol - pos;
        /* 跳过到下一行 */
        u32 next = eol;
        while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;

        if (line_len == 0 || data[pos] == '#') { pos = next; continue; }

        /* 按 '|' 切 4 段：NAME|DISPLAY|PATH|ARGS */
        const char *line = (const char *)data + pos;
        int f0 = -1, f1 = -1, f2 = -1;   /* 三个分隔符位置 */
        for (u32 i = 0; i < line_len; i++) {
            if (line[i] == '|') {
                if (f0 < 0) f0 = (int)i;
                else if (f1 < 0) f1 = (int)i;
                else if (f2 < 0) { f2 = (int)i; break; }
            }
        }
        if (f0 > 0 && f1 > f0) {
            int path_beg = f1 + 1;
            int path_end = (f2 > path_beg) ? f2 : (int)line_len;
            int args_beg = (f2 > path_beg) ? f2 + 1 : (int)line_len;

            lapp_field_copy(g_lapp_name[li], 12, line, f0);
            lapp_field_copy(g_lapp_disp[li], 16, line + f0 + 1, f1 - f0 - 1);
            lapp_field_copy(g_lapp_path[li], 96, line + path_beg, path_end - path_beg);
            if (args_beg < (int)line_len)
                lapp_field_copy(g_lapp_args[li], 96, line + args_beg, (int)line_len - args_beg);
            else
                g_lapp_args[li][0] = 0;

            if (g_lapp_path[li][0] == '/') {
                g_apps[g_app_count] = (app_descriptor){
                    g_lapp_name[li], g_lapp_disp[li], 1024, 768,
                    0, 0, 0, 0, 0,
                    g_lapp_path[li], g_lapp_args[li]
                };
                /* 记录 VSCode app id（IDE attach 共享启动路径用）。
                 * P7.4 匹配收紧：精确匹配 "VSCODE"（第 7 字符须为 NUL），
                 * VSCODE2 等多实例条目不占用共享启动入口。 */
                if (g_vscode_app_id < 0 &&
                    g_lapp_name[li][0] == 'V' && g_lapp_name[li][1] == 'S' &&
                    g_lapp_name[li][2] == 'C' && g_lapp_name[li][3] == 'O' &&
                    g_lapp_name[li][4] == 'D' && g_lapp_name[li][5] == 'E' &&
                    g_lapp_name[li][6] == 0)
                    g_vscode_app_id = g_app_count;
                g_app_count++;
                li++;
            }
        }
        pos = next;
    }
}

/* ---- P5d: PEAPPS.CNF Windows PE 应用注册表 ----
 * FAT32 根目录 PEAPPS.CNF（8.3: PEAPPS CNF），每行一个 Windows PE 应用：
 *   NAME|DISPLAY|path/file.exe|args（'/' 分隔长路径，args 可含空格可省略）
 * '#' 开头为注释。解析结果存静态池，app_descriptor.pe_file 指入。
 * 仅适用于 GUI PE（消息循环驱动 pump）；纯控制台 PE 不会触发消息空转，
 * 窗口模式下桌面将无响应直到进程退出——控制台程序请走 CMD。 */
#define PEAPP_MAX 4
static char g_papp_name[PEAPP_MAX][12];
static char g_papp_disp[PEAPP_MAX][16];
static char g_papp_file[PEAPP_MAX][96];
static char g_papp_args[PEAPP_MAX][96];

static void pe_win_on_draw(void *state, app_ctx *ctx);   /* 定义于 P5d 运行时段 */
static void launch_pe_app(int app_id);

static void register_pe_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83("PEAPPS.CNF", n11) != 0) return;
    if (f32_read_root_file(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

    int pi = 0;
    u32 pos = 0;
    while (pos < size && pi < PEAPP_MAX && g_app_count < MAX_APPS) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 line_len = eol - pos;
        u32 next = eol;
        while (next < size && (data[next] == '\n' || data[next] == '\r')) next++;

        if (line_len == 0 || data[pos] == '#') { pos = next; continue; }

        /* 按 '|' 切 4 段：NAME|DISPLAY|FILE|ARGS */
        const char *line = (const char *)data + pos;
        int f0 = -1, f1 = -1, f2 = -1;
        for (u32 i = 0; i < line_len; i++) {
            if (line[i] == '|') {
                if (f0 < 0) f0 = (int)i;
                else if (f1 < 0) f1 = (int)i;
                else if (f2 < 0) { f2 = (int)i; break; }
            }
        }
        if (f0 > 0 && f1 > f0) {
            int file_beg = f1 + 1;
            int file_end = (f2 > file_beg) ? f2 : (int)line_len;
            int args_beg = (f2 > file_beg) ? f2 + 1 : (int)line_len;

            lapp_field_copy(g_papp_name[pi], 12, line, f0);
            lapp_field_copy(g_papp_disp[pi], 16, line + f0 + 1, f1 - f0 - 1);
            lapp_field_copy(g_papp_file[pi], 96, line + file_beg, file_end - file_beg);
            if (args_beg < (int)line_len)
                lapp_field_copy(g_papp_args[pi], 96, line + args_beg, (int)line_len - args_beg);
            else
                g_papp_args[pi][0] = 0;

            if (g_papp_file[pi][0]) {
                g_apps[g_app_count] = (app_descriptor){
                    g_papp_name[pi], g_papp_disp[pi], 720, 540,
                    0, 0, pe_win_on_draw, 0, 0, 0, 0,
                    g_papp_file[pi], g_papp_args[pi]
                };
                g_app_count++;
                pi++;
            }
        }
        pos = next;
    }
}

static void register_apps(void) {
    /* 命令行1：shell.elf（原生命令行 + 可运行 Linux 程序）*/
    g_apps[0] = (app_descriptor){
        "shell", "Terminal", 720, 480,
        0, 0, 0, 0, "SHELL   ELF"
    };
    g_app_count++;
    /* 命令行2：cmd.elf（Windows 风格命令行 + PE/EXE 执行）*/
    g_apps[1] = (app_descriptor){
        "cmd", "CMD", 720, 480,
        0, 0, 0, 0, "CMD     ELF"
    };
    g_app_count++;
    g_apps[2] = (app_descriptor){
        "editor", "Editor", 600, 450,
        editor_on_create, editor_on_event, editor_on_draw, editor_on_destroy, 0
    };
    g_app_count++;
    g_apps[3] = (app_descriptor){
        "fileman", "Files", 500, 400,
        0, 0, 0, 0, "FILEMAN ELF"
    };
    g_app_count++;
    g_apps[4] = (app_descriptor){
        "calc", "Calculator", 280, 400,
        calc_on_create, calc_on_event, calc_on_draw, calc_on_destroy, 0
    };
    g_app_count++;
    g_apps[5] = (app_descriptor){
        "browser", "Browser", 700, 500,
        0, 0, 0, 0, "BROWSER ELF"
    };
    g_app_count++;
    /* ProEdit — 增强版文本编辑器（语法高亮、设置、目录浏览）*/
    g_apps[6] = (app_descriptor){
        "proedit", "ProEdit", 750, 550,
        0, 0, 0, 0, "PROEDIT ELF"
    };
    g_app_count++;
    /* Settings — 系统设置（内核 FUCK 配置 + 网络 NETCONF 配置）*/
    g_apps[7] = (app_descriptor){
        "settings", "Settings", 760, 540,
        0, 0, 0, 0, "SETTINGSELF"
    };
    g_app_count++;
    /* VSCode Phase 5: Linux guest 应用（VSCode 等），来自 LINUXAPP.CNF */
    register_linux_apps();
    /* P5d: Windows PE 应用，来自 PEAPPS.CNF */
    register_pe_apps();
}

/* Winux-Kate .desktop-grid：canvas padding 24px，列宽 104px，gap 18px，
 * 左→右排列，超出画布宽度则换行（align-content: start）
 *
 * 注意：Terminal 和 CMD 已在第一页 Dashboard（TERM-01 / TERM-02），
 * 桌面不再重复显示其图标。
 */
static void setup_desktop_icons(void) {
    int pad = KATE_ICON_PAD;                        /* 24 */
    int x = pad;
    int y = KATE_TOPBAR_H + KATE_PAGE_PAD + pad;
    int max_x = (int)g_fb_w - pad;                  /* canvas 右缘 */
    for (int i = 0; i < g_app_count; i++) {
        if (g_icon_count >= MAX_ICONS) break;
        /* P7.4: Linux 应用（linux_path）也要出图标——否则 VSCODE 不可见 */
        if (!g_apps[i].on_create && !g_apps[i].elf_name &&
            !g_apps[i].linux_path && !g_apps[i].pe_file) continue;
        /* 跳过 Terminal 和 CMD（已在 Dashboard 第一页） */
        if (g_apps[i].name[0] == 's' && g_apps[i].name[1] == 'h' && g_apps[i].name[2] == 'e') continue;
        if (g_apps[i].name[0] == 'c' && g_apps[i].name[1] == 'm' && g_apps[i].name[2] == 'd') continue;
        if (x > pad && x + KATE_ICON_W > max_x) {
            x = pad;
            y += KATE_ICON_H + KATE_ICON_GAP;
        }
        g_icons[g_icon_count].app_id = i;
        g_icons[g_icon_count].x = x;
        g_icons[g_icon_count].y = y;
        g_icons[g_icon_count].label = g_apps[i].display_name;
        g_icons[g_icon_count].selected = 0;
        g_icon_count++;
        x += KATE_ICON_W + KATE_ICON_GAP;
    }
}

/* ============================================================
 *  外部 ELF 加载
 * ============================================================ */

#define DT_ELFCLASS64 2
#define DT_EM_X86_64 62
#define DT_PT_LOAD 1
#define DT_PT_DYNAMIC 2
#define DT_DT_RELA 7
#define DT_DT_RELASZ 8
#define DT_R_X86_64_RELATIVE 8

typedef struct { u8 ident[16]; u16 type,machine; u32 ver; u64 entry,phoff,shoff; u32 flags; u16 ehsize,phentsize,phnum,shentsize,shnum,shstrndx; } dt_elf64_ehdr;
typedef struct { u32 type,flags; u64 offset,vaddr,paddr,filesz,memsz,align; } dt_elf64_phdr;
typedef struct { i64 tag; u64 val; } dt_elf64_dyn;
typedef struct { u64 offset; u64 info; i64 addend; } dt_elf64_rela;

static u8 g_elf_image[1048576];   /* 1 MiB：settings.elf 内存映像 ~493KB（中文字体位图 BSS），256KB 不够 */

static int dt_load_elf(u8 *data, u32 data_size, void **entry_out) {
    const dt_elf64_ehdr *eh = (const dt_elf64_ehdr *)data;
    if (data_size < sizeof(dt_elf64_ehdr)) return -1;
    if (eh->ident[0] != 0x7F || eh->ident[4] != DT_ELFCLASS64) return -2;
    if (eh->machine != DT_EM_X86_64) return -3;
    u64 min_vaddr = ~0ULL, max_vaddr = 0;
    u32 lc = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const dt_elf64_phdr *ph = (const dt_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != DT_PT_LOAD) continue;
        if (ph->filesz > ph->memsz) return -4;
        if (ph->vaddr < min_vaddr) min_vaddr = ph->vaddr;
        if (ph->vaddr + ph->memsz > max_vaddr) max_vaddr = ph->vaddr + ph->memsz;
        lc++;
    }
    if (!lc || min_vaddr == ~0ULL) return -5;
    u64 isize = (max_vaddr - min_vaddr + 0xFFF) & ~0xFFFULL;
    if (isize > sizeof(g_elf_image)) return -6;
    u8 *image = g_elf_image;
    for (u64 i = 0; i < isize; i++) image[i] = 0;
    for (u16 i = 0; i < eh->phnum; i++) {
        const dt_elf64_phdr *ph = (const dt_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != DT_PT_LOAD) continue;
        u64 off = ph->vaddr - min_vaddr;
        for (u64 j = 0; j < ph->filesz; j++)
            image[off + j] = data[ph->offset + j];
    }
    for (u16 i = 0; i < eh->phnum; i++) {
        const dt_elf64_phdr *ph = (const dt_elf64_phdr *)(data + eh->phoff + (u64)i * eh->phentsize);
        if (ph->type != DT_PT_DYNAMIC) continue;
        const dt_elf64_dyn *dyn = (const dt_elf64_dyn *)(data + ph->offset);
        u64 rela_off = 0, rela_sz = 0;
        for (u32 j = 0; j < ph->filesz / sizeof(dt_elf64_dyn); j++) {
            if (dyn[j].tag == DT_DT_RELA) rela_off = dyn[j].val;
            else if (dyn[j].tag == DT_DT_RELASZ) rela_sz = dyn[j].val;
            else if (dyn[j].tag == 0) break;
        }
        if (rela_off && rela_sz) {
            const dt_elf64_rela *r = (const dt_elf64_rela *)(data + rela_off);
            u32 rn = (u32)(rela_sz / sizeof(dt_elf64_rela));
            u64 load_bias = (u64)image - min_vaddr;   /* 与 DSK loader 一致 */
            for (u32 j = 0; j < rn; j++) {
                u32 type = (u32)(r[j].info & 0xFFFFFFFF);
                if (type == DT_R_X86_64_RELATIVE) {
                    /* 原 BUG: val = min_vaddr + addend，指针被写成低地址绝对值，
                     * 导致 settings.elf 等模块内 .data 中的字符串指针全部失效 */
                    if (r[j].offset < min_vaddr || r[j].offset >= min_vaddr + isize) continue;
                    u64 target = r[j].offset - min_vaddr;
                    u64 val = load_bias + (u64)r[j].addend;
                    *(u64 *)(image + target) = val;
                }
            }
        }
    }
    *entry_out = (void *)(image + (eh->entry - min_vaddr));
    return 0;
}

static void launch_external_elf(const char *name11) {
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(name11, &data, &size) != 0) {
        slog("elf not found");
        return;
    }
    void *entry = 0;
    if (dt_load_elf(data, size, &entry) != 0) {
        slog("elf load failed");
        return;
    }
    slog("launching external elf");
    void (*elf_entry)(const dsk_boot_context *) = (void (*)(const dsk_boot_context *))entry;
    elf_entry(g_boot_ctx);
    g_cursor_saved = 0;
    g_cursor_old_x = -1; g_cursor_old_y = -1;
    files_refresh();          /* 外部应用可能改动了磁盘 */
    redraw_all();
    slog("external elf returned");
}

/* ============================================================
 *  应用启动（单实例聚焦策略）
 * ============================================================ */

static void fill_app_ctx(app_ctx *ctx, desktop_window *win) {
    ctx->fb = &g_fb;
    ctx->win = win;
    ctx->client_x = win->x + BORDER_W;
    ctx->client_y = win->y + KATE_TITLEBAR_H;
    ctx->client_w = win->w - 2 * BORDER_W;
    ctx->client_h = win->h - KATE_TITLEBAR_H - BORDER_W;
    ctx->should_exit = 0;
    ctx->block_read = g_block_read;
    ctx->block_write = g_block_write;
    ctx->kernel_api = g_kernel_api;
}

static void launch_linux_app(int app_id);   /* VSCode Phase 5（定义于 IDE 区） */

static void launch_app(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return;
    app_descriptor *app = &g_apps[app_id];

    /* VSCode Phase 5: Linux guest 应用走 exec_async 共享启动路径 */
    if (app->linux_path) {
        launch_linux_app(app_id);
        return;
    }

    /* P5d: Windows PE 应用走 run_windowed 桌面嵌入路径 */
    if (app->pe_file) {
        launch_pe_app(app_id);
        return;
    }

    if (app->elf_name) {
        launch_external_elf(app->elf_name);
        return;
    }
    if (!app->on_create) return;

    /* 单实例：已有同类窗口则聚焦并迁移到当前工作区 */
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].app_id == app_id && g_windows[i].visible) {
            g_windows[i].ws = g_ws_cur;
            win_focus(&g_windows[i]);
            slog("focus existing app window");
            return;
        }
    }

    int top = KATE_TOPBAR_H, bot = (int)g_fb_h - KATE_TASKBAR_H;
    int wx = ICON_PAD + g_win_count * 30;
    int wy = top + 8 + g_win_count * 30;
    if (wx + (int)app->default_w > (int)g_fb_w - 20) wx = ICON_PAD;
    if (wy + (int)app->default_h > bot - 20) wy = top + 8;

    desktop_window *win = win_create(app_id, app->display_name,
                                      wx, wy, (int)app->default_w, (int)app->default_h);
    if (!win) return;

    app_ctx ctx;
    fill_app_ctx(&ctx, win);

    win->app_state = app->on_create(&ctx);
    win->dirty = 1;

    /* 直接显示窗口（去掉打开动画以避免卡顿和闪屏） */

    slog("app launched");
}

/* ============================================================
 *  FILES 面板（FAT32 真实读写）
 * ============================================================ */

static int files_list_cb(const char *name, u32 size, u8 attr, void *ud) {
    (void)ud;
    if (attr & 0x10) return 0;                 /* 跳过子目录 */
    if (g_file_count >= MAX_FILES) return 1;   /* 停止 */

    /* 只显示 user 文件：跳过 .ELF 可执行文件和系统模块 */
    int nl = kstrlen(name);
    if (nl >= 4 && name[nl-4] == '.' && name[nl-3] == 'E' &&
        name[nl-2] == 'L' && name[nl-1] == 'F') return 0;
    /* FIRSTINI* (FirstInit.elf，Name83=FIRSTINIT → 显示名 FIRSTINI.T) */
    if (nl >= 8 && name[0] == 'F' && name[1] == 'I' && name[2] == 'R' &&
        name[3] == 'S' && name[4] == 'T' && name[5] == 'I' && name[6] == 'N' &&
        name[7] == 'I') return 0;

    fentry *f = &g_files[g_file_count];
    f->size = size;
    kstrcpy(f->disp, name, 13);
    /* disp (NAME.EXT) → name11 (NAME    EXT) */
    for (int i = 0; i < 11; i++) f->name11[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) f->name11[o++] = name[i++];
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        o = 8;
        while (name[i] && o < 11) f->name11[o++] = name[i++];
    }
    g_file_count++;
    return 0;
}

static void files_refresh(void) {
    g_file_count = 0;
    g_file_sel = -1;
    if (!g_block_read) { g_files_loaded = 0; return; }
    if (f32_list_root(files_list_cb, 0) == 0) g_files_loaded = 1;
    else g_files_loaded = 0;
}

/* 双击打开文件 → 载入 DASHBOARD EDITOR */
static void files_open(int idx) {
    if (idx < 0 || idx >= g_file_count) return;
    editor_state *ed = (editor_state *)ADDR_EDDASH;
    fentry *f = &g_files[idx];

    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(f->name11, &data, &size) != 0) {
        slog("file open failed");
        return;
    }
    ed->text_len = 0;
    u32 cap = EDITOR_BUF_SIZE - 1;
    if (size > cap) size = cap;
    for (u32 i = 0; i < size; i++) {
        char c = (char)data[i];
        if ((u8)c >= 32 || c == '\n' || c == '\t')
            ed->text[ed->text_len++] = c;
    }
    for (int i = 0; i < 11; i++) ed->name11[i] = f->name11[i];
    kstrcpy(ed->disp, f->disp, 13);
    ed->has_file = 1;
    ed->modified = 0;
    ed->scroll_y = 0;
    ed->cursor_pos = 0;
    editor_recalc_cursor(ed);
    g_dash_focus = 2;
    slog("file loaded into editor");
}

/* EDITOR SAVE 回写磁盘 */
static void editor_save(void) {
    editor_state *ed = (editor_state *)ADDR_EDDASH;
    if (!ed->has_file) return;
    if (f32_write_root_file(ed->name11, (const u8 *)ed->text, (u32)ed->text_len) == 0) {
        ed->modified = 0;
        slog("file saved");
    } else {
        slog("file save failed");
    }
}

/* ============================================================
 *  通用小绘制
 * ============================================================ */

/* 矩形内居中文字 */
static void draw_centered(const char *s, int cx, int y, u32 fg, u32 bg) {
    int l = kstrlen(s);
    du_draw_string(&g_fb, s, cx - l * (int)DU_ASCII_STEP / 2, y, fg, bg, DU_ASCII_STEP);
}

/* 虚线矩形（ide-host / custom host） */
static void draw_dashed_rect(int x, int y, int w, int h, u32 color) {
    for (int i = x; i < x + w; i += 8) {
        du_fill_rect(&g_fb, i, y, 4, 1, color);
        du_fill_rect(&g_fb, i, y + h - 1, 4, 1, color);
    }
    for (int i = y; i < y + h; i += 8) {
        du_fill_rect(&g_fb, x, i, 1, 4, color);
        du_fill_rect(&g_fb, x + w - 1, i, 1, 4, color);
    }
}

/* ============================================================
 *  Boot 屏（打字机）
 * ============================================================ */

static const char *BOOT_LINES[] = {
    "DESHAB SHELL v0.1.0",
    "initializing UTSM sealed memory...",
    "mounting FAT32 root filesystem............[ OK ]",
    "loading window manager (SAS-R0 Ring0).....[ OK ]",
    "registering PS/2 input drivers............[ OK ]",
    "probing AHCI block device.................[ OK ]",
    "resolving desktop shortcuts...............[ OK ]",
    "desktop replaced by Winux-Kate shell......[ OK ]",
    "shell ready.",
    "DEAICUP STUDIO",
};
#define BOOT_LINE_COUNT 10

static void busy_delay(u32 cycles) {
    for (volatile u32 i = 0; i < cycles; i++) __asm__("pause");
}

static void boot_screen(void) {
    int W = (int)g_fb_w, H = (int)g_fb_h;
    du_fill_bg_solid(&g_fb, KS_BG_PRIMARY);
    flip_buffer();

    int lw = 46 * (int)DU_ASCII_STEP;
    int lx = (W - lw) / 2;
    int ty = H / 2 - 180;

    /* boot-title */
    const char *title = "DESHAB";
    int tl = kstrlen(title);
    for (int i = 0; i < tl; i++) {
        char t[2] = { title[i], 0 };
        du_draw_string(&g_fb, t, W / 2 - tl * (int)DU_ASCII_STEP + i * (int)DU_ASCII_STEP * 2,
                       ty, KS_ACCENT, 0, DU_ASCII_STEP * 2);
        flip_buffer();
        busy_delay(2000000);
    }
    du_divider_h(&g_fb, W / 2 - 120, ty + 30, 240, KS_ACCENT);

    /* boot-subtitle（对应 Kate "由 Deaicup 工作室制作"） */
    draw_centered("DEAICUP STUDIO", W / 2, ty + 44, KS_ACCENT2, 0);
    flip_buffer();
    busy_delay(2500000);

    /* boot-lines 打字机；进度条固定在全部行下方（对应 Kate .boot-bar） */
    int ly = ty + 76;
    int bar_y = ly + BOOT_LINE_COUNT * ((int)DU_ASCII_LINE_H + 2) + 12;
    int bw = 260;
    for (int li = 0; li < BOOT_LINE_COUNT; li++) {
        const char *s = BOOT_LINES[li];
        u32 fg = (li == 1 || li == BOOT_LINE_COUNT - 1) ? KS_TEXT_DIM : KS_ACCENT2;
        du_draw_string(&g_fb, ">", lx, ly, KS_ACCENT, 0, DU_ASCII_STEP);
        for (int ci = 0; s[ci]; ci++) {
            char t[2] = { s[ci], 0 };
            du_draw_string(&g_fb, t, lx + (ci + 1) * (int)DU_ASCII_STEP, ly,
                           fg, 0, DU_ASCII_STEP);
            flip_buffer();
            busy_delay(125000);
        }
        ly += (int)DU_ASCII_LINE_H + 2;
        /* boot-bar:渐变填充（accent → accent2） */
        int fill = bw * (li + 1) / BOOT_LINE_COUNT;
        du_rect_outline(&g_fb, W / 2 - bw / 2, bar_y, bw, 6, KS_BORDER, 1);
        if (fill > 2) du_fill_rect_gradient(&g_fb, W / 2 - bw / 2 + 1, bar_y + 1,
                                            fill - 2, 4, KS_ACCENT, KS_ACCENT2);
        flip_buffer();
    }

    /* boot-hint + credit（对应 Kate "SYSTEM ONLINE" + "© 2026 Deaicup Studio"） */
    draw_centered("SYSTEM ONLINE", W / 2, H - 64, KS_ACCENT2, 0);
    draw_centered("(C) 2026 DEAICUP STUDIO", W / 2, H - 40, KS_TEXT_DIM, 0);
    flip_buffer();
    busy_delay(10000000);
}

/* ============================================================
 *  顶栏（brand + WorkspaceSwitcher + PageSwitcher + PG 时钟）
 * ============================================================ */

static void draw_topbar(void) {
    int W = (int)g_fb_w;
    du_fill_rect_gradient(&g_fb, 0, 0, W, KATE_TOPBAR_H,
                          0xFF0A2840u, KS_BG_PRIMARY);
    du_divider_h(&g_fb, 0, KATE_TOPBAR_H, W, KS_BORDER);

    int x = 12;
    int ty = (KATE_TOPBAR_H - (int)DU_ASCII_LINE_H) / 2 + 1;

    /* brand：方块点 + DESHAB + brand-credit（对应 Kate "由 Deaicup 工作室制作"） */
    du_fill_rect(&g_fb, x, KATE_TOPBAR_H / 2 - 3, 6, 6, KS_ACCENT);
    du_draw_string(&g_fb, "DESHAB", x + 12, ty, KS_ACCENT, 0, DU_ASCII_STEP + 2);
    x += 12 + 6 * ((int)DU_ASCII_STEP + 2) + 10;
    du_draw_string(&g_fb, "DEAICUP STUDIO", x, ty + 2, KS_ACCENT2, 0, DU_ASCII_STEP);
    x += 14 * (int)DU_ASCII_STEP + 14;

    /* 分隔线 + 任务视图按钮（三横线图标） */
    du_divider_v(&g_fb, x - 6, 6, KATE_TOPBAR_H - 12, KS_BORDER);
    g_tvbtn_rect[0] = x; g_tvbtn_rect[1] = 5;
    g_tvbtn_rect[2] = 28; g_tvbtn_rect[3] = 24;
    {
        int hover = g_mouse_x >= x && g_mouse_x < x + 28 &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (hover) du_fill_rect(&g_fb, x, 5, 28, 24, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, 5, 28, 24, KS_BORDER, 1);
        du_fill_rect(&g_fb, x + 6, 11, 16, 2, KS_ACCENT);
        du_fill_rect(&g_fb, x + 6, 16, 16, 2, KS_ACCENT);
        du_fill_rect(&g_fb, x + 6, 21, 16, 2, KS_ACCENT);
    }
    x += 34;

    /* 工作区按钮 "name|n" */
    for (int i = 0; i < g_ws_count; i++) {
        char label[24];
        kstrcpy(label, g_ws[i].name, 18);
        kstrcat(label, "|", 24);
        int cnt = ws_win_count(i);
        char cn[4];
        cn[0] = '0' + (char)(cnt % 10); cn[1] = 0;
        kstrcat(label, cn, 24);
        int bw = kstrlen(label) * (int)DU_ASCII_STEP + 16;
        int active = (i == g_ws_cur);
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (active) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT2);
            du_kate_glow_border(&g_fb, x, 5, bw, 24, KS_ACCENT2);
        } else if (hover) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT_DIM);
        }
        du_rect_outline(&g_fb, x, 5, bw, 24, active ? KS_ACCENT2 : KS_BORDER, 1);
        du_draw_string(&g_fb, label, x + 8, ty,
                       active ? KS_TEXT_INVERT : KS_TEXT_DIM, 0, DU_ASCII_STEP);
        g_ws_rect[i][0] = x; g_ws_rect[i][1] = 5;
        g_ws_rect[i][2] = bw; g_ws_rect[i][3] = 24;
        x += bw + 6;
    }

    /* ws-add "+" */
    g_wsadd_rect[0] = x; g_wsadd_rect[1] = 5;
    g_wsadd_rect[2] = 26; g_wsadd_rect[3] = 24;
    {
        int hover = g_mouse_x >= x && g_mouse_x < x + 26 &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (hover) du_fill_rect(&g_fb, x, 5, 26, 24, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, 5, 26, 24, KS_ACCENT2, 1);
        du_draw_string(&g_fb, "+", x + 7, ty - 1, KS_ACCENT2, 0, DU_ASCII_STEP);
    }
    x += 32;

    /* 分隔线 */
    du_divider_v(&g_fb, x - 4, 6, KATE_TOPBAR_H - 12, KS_BORDER);
    x += 4;

    /* 页面切换：精简按钮文字 */
    static const char *pg_names[3] = { "DASH", "IDE", "DESK" };
    for (int i = 0; i < 3; i++) {
        int bw = kstrlen(pg_names[i]) * (int)DU_ASCII_STEP + 24;
        int active = (g_page == i + 1);
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (active) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT);
            du_kate_glow_border(&g_fb, x, 5, bw, 24, KS_ACCENT);
        } else if (hover) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT_DIM);
        }
        du_rect_outline(&g_fb, x, 5, bw, 24, active ? KS_ACCENT : KS_BORDER, 1);
        du_draw_string(&g_fb, pg_names[i], x + 12, ty,
                       active ? KS_TEXT_INVERT : KS_TEXT_DIM, 0, DU_ASCII_STEP);
        g_pg_rect[i][0] = x; g_pg_rect[i][1] = 5;
        g_pg_rect[i][2] = bw; g_pg_rect[i][3] = 24;
        x += bw + 8;
    }
    /* 自定义页 */
    for (int i = 0; i < g_cpage_count; i++) {
        char label[20];
        label[0] = '0' + (char)g_cpages[i].id;
        label[1] = '.'; label[2] = 0;
        kstrcat(label, g_cpages[i].name, 20);
        int bw = kstrlen(label) * (int)DU_ASCII_STEP + 28;
        int active = (g_page == g_cpages[i].id);
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (active) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT);
        } else if (hover) {
            du_fill_rect(&g_fb, x, 5, bw, 24, KS_ACCENT_DIM);
        }
        du_rect_outline(&g_fb, x, 5, bw, 24, active ? KS_ACCENT : KS_BORDER, 1);
        du_draw_string(&g_fb, label, x + 6, ty,
                       active ? KS_TEXT_INVERT : KS_TEXT_DIM, 0, DU_ASCII_STEP);
        du_draw_string(&g_fb, "x", x + bw - 14, ty, KS_DANGER, 0, DU_ASCII_STEP);
        g_pg_rect[3 + i][0] = x; g_pg_rect[3 + i][1] = 5;
        g_pg_rect[3 + i][2] = bw; g_pg_rect[3 + i][3] = 24;
        g_pg_rm_rect[3 + i][0] = x + bw - 20; g_pg_rm_rect[3 + i][1] = 5;
        g_pg_rm_rect[3 + i][2] = 20; g_pg_rm_rect[3 + i][3] = 24;
        x += bw + 6;
    }
    /* pg-add "+" */
    if (g_cpage_count < MAX_CPAGES) {
        g_pgadd_rect[0] = x; g_pgadd_rect[1] = 5;
        g_pgadd_rect[2] = 26; g_pgadd_rect[3] = 24;
        int hover = g_mouse_x >= x && g_mouse_x < x + 26 &&
                    g_mouse_y >= 5 && g_mouse_y < 29;
        if (hover) du_fill_rect(&g_fb, x, 5, 26, 24, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, 5, 26, 24, KS_ACCENT2, 1);
        du_draw_string(&g_fb, "+", x + 7, ty - 1, KS_ACCENT2, 0, DU_ASCII_STEP);
    } else {
        g_pgadd_rect[2] = 0;
    }

    /* 右侧：credit-top + PG 时钟（对应 Kate "© Deaicup Studio" + "PG n"） */
    char pgclk[8];
    pgclk[0] = 'P'; pgclk[1] = 'G'; pgclk[2] = ' ';
    pgclk[3] = '0' + (char)(g_page % 10); pgclk[4] = 0;
    const char *credit = "(C) DEAICUP STUDIO";
    int cl = kstrlen(credit) + 1 + kstrlen(pgclk);
    du_draw_string(&g_fb, credit, W - 12 - cl * (int)DU_ASCII_STEP, ty,
                   KS_TEXT_DIM, 0, DU_ASCII_STEP);
    du_draw_string(&g_fb, pgclk, W - 12 - kstrlen(pgclk) * (int)DU_ASCII_STEP, ty,
                   KS_ACCENT2, 0, DU_ASCII_STEP);
}

/* ============================================================
 *  滑块（VOL / BRIGHT）
 * ============================================================ */

static void draw_slider(int x, int y, int w, int val, int rect_out[4]) {
    du_fill_rect(&g_fb, x, y + 5, w, 3, KS_BG_TERTIARY);
    du_rect_outline(&g_fb, x, y + 4, w, 5, KS_BORDER, 1);
    int fw = w * val / 100;
    if (fw > 0) du_fill_rect(&g_fb, x, y + 5, fw, 3, KS_ACCENT);
    int kx = x + fw - 3;
    if (kx < x) kx = x;
    if (kx > x + w - 6) kx = x + w - 6;
    du_fill_rect(&g_fb, kx, y, 6, 13, KS_ACCENT);
    du_rect_outline(&g_fb, kx, y, 6, 13, KS_ACCENT, 1);
    rect_out[0] = x; rect_out[1] = y - 2;
    rect_out[2] = w; rect_out[3] = 17;
}

/* ============================================================
 *  状态栏（DASHBOARD 底部 32px）
 * ============================================================ */

static void draw_statusbar_full(void) {
    int W = (int)g_fb_w;
    int y = (int)g_fb_h - KATE_STATUSBAR_H;
    du_fill_rect(&g_fb, 0, y, W, KATE_STATUSBAR_H, 0xFF051828u);
    du_divider_h(&g_fb, 0, y, W, KS_BORDER);

    int x = 12;
    int ty = y + (KATE_STATUSBAR_H - (int)DU_ASCII_LINE_H) / 2 + 1;

    /* 时钟（日期 · 秒级时间，accent2） */
    int yy, mo, dd, hh, mi, ss;
    rtc_read(&yy, &mo, &dd, &hh, &mi, &ss);
    char clk[32];
    int cp2 = 0;
    clk[cp2++] = '2'; clk[cp2++] = '0';
    clk[cp2++] = '0' + (char)(yy / 10); clk[cp2++] = '0' + (char)(yy % 10);
    clk[cp2++] = '-';
    clk[cp2++] = '0' + (char)(mo / 10); clk[cp2++] = '0' + (char)(mo % 10);
    clk[cp2++] = '-';
    clk[cp2++] = '0' + (char)(dd / 10); clk[cp2++] = '0' + (char)(dd % 10);
    clk[cp2++] = ' '; clk[cp2++] = '.'; clk[cp2++] = ' ';
    char hms[9];
    fmt_hms(hms, hh * 3600 + mi * 60 + ss);
    for (int i = 0; hms[i]; i++) clk[cp2++] = hms[i];
    clk[cp2] = 0;
    du_draw_string(&g_fb, clk, x, ty, KS_ACCENT2, 0, DU_ASCII_STEP);
    x += cp2 * (int)DU_ASCII_STEP + 18;

    /* UP 计时 */
    {
        int up = hh * 3600 + mi * 60 + ss - g_rtc_boot_sec;
        if (up < 0) up += 86400;
        char upb[16];
        kstrcpy(upb, "UP ", 16);
        char uh[9];
        fmt_hms(uh, up);
        kstrcat(upb, uh, 16);
        du_draw_string(&g_fb, upb, x, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);
        x += kstrlen(upb) * (int)DU_ASCII_STEP + 18;
    }

    /* VOL 滑块 */
    du_draw_string(&g_fb, "VOL", x, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);
    x += 3 * (int)DU_ASCII_STEP + 6;
    draw_slider(x, y + 9, 90, g_vol, g_vol_rect);
    x += 90 + 8;
    {
        char vb[6];
        vb[0] = '0' + (char)(g_vol / 100 % 10);
        vb[1] = '0' + (char)(g_vol / 10 % 10);
        vb[2] = '0' + (char)(g_vol % 10);
        vb[3] = '%'; vb[4] = 0;
        const char *vp = (g_vol >= 100) ? vb : (g_vol >= 10 ? vb + 1 : vb + 2);
        du_draw_string(&g_fb, vp, x, ty, KS_ACCENT2, 0, DU_ASCII_STEP);
        x += 4 * (int)DU_ASCII_STEP + 14;
    }

    /* BRIGHT 滑块 */
    du_draw_string(&g_fb, "BRIGHT", x, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);
    x += 6 * (int)DU_ASCII_STEP + 6;
    draw_slider(x, y + 9, 90, g_bright, g_bri_rect);
    x += 90 + 8;
    {
        char vb[6];
        vb[0] = '0' + (char)(g_bright / 100 % 10);
        vb[1] = '0' + (char)(g_bright / 10 % 10);
        vb[2] = '0' + (char)(g_bright % 10);
        vb[3] = '%'; vb[4] = 0;
        const char *vp = (g_bright >= 100) ? vb : (g_bright >= 10 ? vb + 1 : vb + 2);
        du_draw_string(&g_fb, vp, x, ty, KS_ACCENT2, 0, DU_ASCII_STEP);
        x += 4 * (int)DU_ASCII_STEP + 14;
    }

    /* BT / WIFI */
    du_draw_string(&g_fb, "BT OFF", x, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);
    x += 6 * (int)DU_ASCII_STEP + 14;
    du_draw_string(&g_fb, "WIFI --", x, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* 右侧 hint */
    const char *hint = "Ctrl+Tab PAGE";
    du_draw_string(&g_fb, hint, W - 12 - kstrlen(hint) * (int)DU_ASCII_STEP, ty,
                   KS_TEXT_DIM, 0, DU_ASCII_STEP);
}

/* ============================================================
 *  页面绘制
 * ============================================================ */

static void draw_desktop_icons(void) {
    /* desktop-empty（Kate：无快捷方式提示） */
    if (g_icon_count == 0) {
        du_draw_string(&g_fb, "NO SHORTCUTS ON DESKTOP",
                       KATE_ICON_PAD, KATE_TOPBAR_H + KATE_PAGE_PAD + KATE_ICON_PAD + 16,
                       KS_TEXT_DIM, 0, DU_ASCII_STEP);
        return;
    }
    for (int i = 0; i < g_icon_count; i++) {
        desktop_icon *ic = &g_icons[i];
        du_kate_desktop_icon(&g_fb, ic->x, ic->y, ic->label, ic->selected);
        int ix = ic->x + (KATE_ICON_W - KATE_ICON_IMG) / 2;
        int iy = ic->y + 10;
        draw_app_icon(&g_fb, ic->app_id, ix, iy);
    }
}

/* ---- 1·DASHBOARD ---- */
static void dash_layout(void) {
    int top = KATE_TOPBAR_H, bot = (int)g_fb_h - KATE_STATUSBAR_H;
    int pad = 8, gap = 8;
    int avail_w = (int)g_fb_w - 2 * pad;
    int avail_h = bot - top - 2 * pad;
    int left_w = avail_w * 35 / 100;   /* 终端面板：42%→35% */
    int right_x = pad + left_w + gap;
    int right_w = (int)g_fb_w - pad - right_x;
    int row_h = (avail_h - gap) / 2;

    g_dash_rect[0][0] = pad;     g_dash_rect[0][1] = top + pad;
    g_dash_rect[0][2] = left_w;  g_dash_rect[0][3] = row_h;
    g_dash_rect[1][0] = pad;     g_dash_rect[1][1] = top + pad + row_h + gap;
    g_dash_rect[1][2] = left_w;  g_dash_rect[1][3] = row_h;
    g_dash_rect[2][0] = right_x; g_dash_rect[2][1] = top + pad;
    g_dash_rect[2][2] = right_w; g_dash_rect[2][3] = row_h;
    g_dash_rect[3][0] = right_x; g_dash_rect[3][1] = top + pad + row_h + gap;
    g_dash_rect[3][2] = right_w; g_dash_rect[3][3] = row_h;
}

static void draw_dashboard(void) {
    dash_layout();
    const char *headers[4] = { "TERM-01", "TERM-02", 0, "FILES" };
    char ed_hdr[40];
    editor_state *ed = (editor_state *)ADDR_EDDASH;
    kstrcpy(ed_hdr, "EDITOR . ", 40);
    kstrcat(ed_hdr, ed->has_file ? ed->disp : "untitled", 40);
    headers[2] = ed_hdr;

    for (int i = 0; i < 4; i++) {
        int x = g_dash_rect[i][0], y = g_dash_rect[i][1];
        int w = g_dash_rect[i][2], h = g_dash_rect[i][3];
        du_kate_panel(&g_fb, x, y, w, h, headers[i]);
        if (g_dash_focus == i) {
            du_rect_outline(&g_fb, x, y, w, h, KS_ACCENT, 2);
        }
    }

    /* EDITOR 头 SAVE 按钮（仅关联文件后可用） */
    {
        int ex = g_dash_rect[2][0], ey = g_dash_rect[2][1], ew = g_dash_rect[2][2];
        int bw = 52, bh = 18;
        int bx = ex + ew - bw - 6, by = ey + 3;
        g_save_rect[0] = bx; g_save_rect[1] = by;
        g_save_rect[2] = bw; g_save_rect[3] = bh;
        u32 fg = ed->has_file ? KS_ACCENT : KS_TEXT_DIM;
        int hover = ed->has_file &&
                    g_mouse_x >= bx && g_mouse_x < bx + bw &&
                    g_mouse_y >= by && g_mouse_y < by + bh;
        if (hover) du_fill_rect(&g_fb, bx, by, bw, bh, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, bx, by, bw, bh, ed->has_file ? KS_BORDER : KS_BORDER_DIM, 1);
        du_draw_string(&g_fb, "SAVE", bx + 5, by + 1, fg, 0, DU_ASCII_STEP);
    }

    /* 面板内容 */
    bash_draw_to((bash_state *)ADDR_TERM1,
                 g_dash_rect[0][0] + 2, g_dash_rect[0][1] + KATE_PANEL_HEADER_H + 2,
                 g_dash_rect[0][2] - 4, g_dash_rect[0][3] - KATE_PANEL_HEADER_H - 4);

    bash_draw_to((bash_state *)ADDR_TERM2,
                 g_dash_rect[1][0] + 2, g_dash_rect[1][1] + KATE_PANEL_HEADER_H + 2,
                 g_dash_rect[1][2] - 4, g_dash_rect[1][3] - KATE_PANEL_HEADER_H - 4);

    editor_draw_to(ed,
                   g_dash_rect[2][0] + 2, g_dash_rect[2][1] + KATE_PANEL_HEADER_H + 2,
                   g_dash_rect[2][2] - 4, g_dash_rect[2][3] - KATE_PANEL_HEADER_H - 4);

    /* FILES 面板内容 */
    {
        int fx = g_dash_rect[3][0] + 2, fy = g_dash_rect[3][1] + KATE_PANEL_HEADER_H + 2;
        int fw = g_dash_rect[3][2] - 4, fh = g_dash_rect[3][3] - KATE_PANEL_HEADER_H - 4;
        du_fill_rect(&g_fb, fx, fy, fw, fh, KS_BG_PRIMARY);

        char path_buf[32];
        kstrcpy(path_buf, "FAT32:/USER (", 32);
        int pl = kstrlen(path_buf);
        if (g_file_count >= 10) path_buf[pl++] = '0' + (char)(g_file_count / 10);
        path_buf[pl++] = '0' + (char)(g_file_count % 10);
        path_buf[pl++] = ')'; path_buf[pl] = 0;
        du_draw_string(&g_fb, path_buf, fx + 8, fy + 3, KS_ACCENT, KS_BG_PRIMARY, DU_ASCII_STEP);
        du_divider_h(&g_fb, fx, fy + 22, fw, KS_BORDER);

        int row_h = 22;
        int max_rows = (fh - 24) / row_h;
        for (int i = 0; i < g_file_count && i < max_rows; i++) {
            int ry = fy + 24 + i * row_h;
            if (i == g_file_sel) {
                du_fill_rect(&g_fb, fx, ry, fw, row_h, KS_ACCENT_DIM);
            }
            int hover = (g_mouse_x >= fx && g_mouse_x < fx + fw &&
                         g_mouse_y >= ry && g_mouse_y < ry + row_h);
            if (hover && i != g_file_sel) {
                du_fill_rect(&g_fb, fx, ry, fw, row_h, KS_ACCENT_DIM);
            }
            u32 fg = (i == g_file_sel) ? KS_ACCENT : KS_TEXT_PRIMARY;
            du_draw_string(&g_fb, g_files[i].disp, fx + 8, ry + 2, fg, 0, DU_ASCII_STEP);
            char sz[12];
            u32 kb = g_files[i].size / 1024;
            int sp = 0;
            if (kb >= 100) { sz[sp++] = '0' + (char)(kb / 100 % 10); }
            if (kb >= 10)  { sz[sp++] = '0' + (char)(kb / 10 % 10); }
            sz[sp++] = '0' + (char)(kb % 10);
            sz[sp++] = 'K'; sz[sp] = 0;
            du_draw_string(&g_fb, sz, fx + fw - sp * (int)DU_ASCII_STEP - 8, ry + 2,
                           KS_TEXT_DIM, 0, DU_ASCII_STEP);
        }
        if (!g_files_loaded) {
            du_draw_string(&g_fb, "NO BLOCK DEVICE", fx + 8, fy + 28,
                           KS_DANGER, KS_BG_PRIMARY, DU_ASCII_STEP);
        } else if (g_file_count == 0) {
            du_draw_string(&g_fb, "(empty)", fx + 8, fy + 28,
                           KS_TEXT_DIM, KS_BG_PRIMARY, DU_ASCII_STEP);
        }
    }

    draw_statusbar_full();
}

/* ---- 2·IDE ---- */
static int g_ide_attached = 0;
static void page_switch(int p);   /* 定义于工作区段，launch_linux_app 前向引用 */

/* VSCode Phase 6: guest GUI 状态机。
 * 0 = 未运行（IDE host 显示启动提示）
 * 1 = 启动中（X server/VSCode spawn 已发出，等 scanout 上线）
 * 2 = 运行中（scanout enabled，blit guest 帧） */
static int g_vscode_state = 0;

/* P8.1: VSCode 启动插桩--分阶段状态跟踪 + 串口日志 + 屏幕显示。
 * g_launch_phase: 0=idle 1=X启动 2=X socket等待 3=app spawn 4=pgrep验证 5=等scanout 6=running 7=失败
 * g_launch_status: 屏幕显示的状态字符串（IDE host 区域） */
static int g_launch_phase = 0;
static char g_launch_status[64] = "";
static u64 g_launch_tsc = 0;        /* 启动开始 TSC（测各阶段耗时） */
static int g_launch_exec_rc = 0;    /* exec_async 返回码 */
static int g_launch_pgrep_rc = -1;  /* pgrep 验证结果（0=找到, 1=未找到）*/

/* P7.4 多实例：IDE tab 动态标签——显示最近启动实例的 DISPLAY 名
 * （单 X :0 共享 scanout，多 VSCode 进程同屏；标签指示最后 attach 者）。 */
static char g_ide_tab_label[16] = "VSCODE";

/* P7.9: 性能 profiling（TSC 计时，每秒更新 FPS 显示）。
 * 在 IDE 页面叠加显示 FPS / blit 耗时 / dirty 跳过率。 */
static u64 g_prof_frames = 0;        /* 自上次 FPS 更新以来的帧数 */
static u64 g_prof_fps_tsc = 0;       /* 上次 FPS 计算的 TSC */
static int g_prof_fps = 0;           /* 当前 FPS（每秒更新） */
static u64 g_prof_blit_total = 0;    /* 累计 blit TSC */
static u64 g_prof_blit_count = 0;    /* blit 调用次数 */
static u64 g_prof_blit_skip = 0;     /* dirty=0 跳过的次数 */
static u64 g_prof_blit_full = 0;     /* full repaint 次数 */
static int g_prof_last_blit_us = 0;  /* 上次 blit 耗时（微秒） */

/* P7.3: attach/detach 时使 scanout 缓存失效并请求全量重绘（定义于 blit 区） */
static void ide_attach_invalidate(void);

/* VSCode Phase 3: IDE attach 时在 Linux guest 中启动 X server（Xfbdev）。
 * Xfbdev 通过 virtio-gpu 2D 设备合成帧到 surface pool scanout 影子缓冲，
 * desktop 每帧 blit 到 IDE host 区域。exec 用 sh -c "... &" 后台启动，
 * shell 立即退出被 daemon 回收，Xfbdev 由 init 收养继续运行。
 * Phase 6: pgrep 守卫保证幂等（多实例/重复 attach 不重起 X）。 */
/* P8.1: slog + 数字（用于 rc/ec 日志，负数正确显示前导 -） */
static void slog_num(const char *prefix, int rc) {
    char buf[48];
    kstrcpy(buf, prefix, 48);
    int n = kstrlen(buf);
    if (rc < 0) { buf[n++] = '-'; rc = -rc; }
    u32dec((u32)rc, buf + n);
    slog(buf);
}

static void ide_launch_guest_x(void) {
    if (!g_lxc_svc || !g_lxc_svc->exec) { slog("ide: lxc exec unavailable"); return; }
    slog("[launch] phase 1: launching Xfbdev");
    g_launch_phase = 1;
    kstrcpy(g_launch_status, "LAUNCHING X SERVER . . .", 64);
    const char *argv[] = {
        "sh", "-c",
        "pgrep -x Xfbdev >/dev/null 2>&1 || "
        "Xfbdev :0 -geometry 1024x768 -depth 32 >/dev/null 2>&1 &"
    };
    u64 exit_code = 0;
    int rc = g_lxc_svc->exec("/bin/sh", 3, argv, 0, 0, 0, &exit_code);
    slog_num("[launch] Xfbdev exec rc=", rc);
    if (rc != 0) {
        slog("[launch] WARNING: Xfbdev launch rc!=0 (may still be OK if already running)");
    }
}

/* P8.1 Fix: lxc guest ping -- exec "echo OK" 快速检测 Linux guest 是否运行。
 * 返回 0=guest 可用，非 0=不可用（VMX 未启用/guest 未 park/IPC 不通）。 */
static int lxc_guest_ping(void) {
    if (!g_lxc_svc || !g_lxc_svc->exec) return -1;
    const char *pargv[] = { "sh", "-c", "echo OK" };
    char out[8] = {0};
    u64 ec = 0;
    int rc = g_lxc_svc->exec("/bin/sh", 3, pargv, out, 7, 0, &ec);
    if (rc != 0) return rc;
    if (out[0] == 'O' && out[1] == 'K') return 0;
    return -1;
}

/* VSCode Phase 6: 共享 guest GUI 启动路径（IDE +NEW/双击 host 与桌面图标共用）。
 * 首次调用确保 X server 在线（幂等启动 + 有界等待 X socket），然后
 * exec_async 目标程序（daemon fork+setsid+detach，立即返回）。
 * 后续调用（多实例）跳过 X 准备直接 spawn。path 为 NULL 时仅确保 X 在线。
 * P8.1 Fix: 启动前 ping 检测 guest 可用性；失败时快速短路，不浪费 5s X socket 等待。 */
static void guest_gui_launch(const char *path, int argc, const char *const *argv) {
    if (!g_lxc_svc) {
        slog("[launch] lxc service null");
        g_launch_phase = 7;
        kstrcpy(g_launch_status, "LXC SERVICE UNAVAILABLE", 64);
        return;
    }
    if (g_vscode_state == 0) {
        g_vscode_state = 1;   /* 启动中 */
        g_launch_tsc = rdtsc();
        slog("[launch] === guest GUI launch sequence start ===");

        /* P8.1 Fix: Phase 0 - ping guest 检测可用性 */
        g_launch_phase = 0;
        kstrcpy(g_launch_status, "CHECKING GUEST . . .", 64);
        int prc = lxc_guest_ping();
        slog_num("[launch] guest ping rc=", prc);
        if (prc != 0) {
            slog("[launch] FAIL: Linux guest not running (VMX/guest unavailable)");
            g_vscode_state = 0;   /* 回到未启动状态 */
            g_launch_phase = 7;
            kstrcpy(g_launch_status, "GUEST NOT RUNNING - NEED KVM/VMX", 64);
            return;
        }
        slog("[launch] guest ping OK - Linux guest is running");

        /* Phase 1: launch X server */
        ide_launch_guest_x();

        /* Phase 2: wait for X socket (bounded ~5s) */
        g_launch_phase = 2;
        kstrcpy(g_launch_status, "WAITING FOR X SOCKET . . .", 64);
        slog("[launch] phase 2: waiting for X socket");
        if (g_lxc_svc->exec) {
            const char *wargv[] = {
                "sh", "-c",
                "i=0; while [ ! -S /tmp/.X11-unix/X0 ] && [ $i -lt 25 ]; "
                "do sleep 0.2; i=$((i+1)); done; [ -S /tmp/.X11-unix/X0 ] && echo OK || echo FAIL"
            };
            /* 用 exec + stdout 捕获验证 X socket 是否就绪 */
            char xout[16] = {0};
            u64 ec = 0;
            int rc = g_lxc_svc->exec("/bin/sh", 3, wargv, xout, 15, 0, &ec);
            slog_num("[launch] X socket wait rc=", rc);
            if (xout[0] == 'O' && xout[1] == 'K') {
                slog("[launch] X socket READY (/tmp/.X11-unix/X0 exists)");
            } else {
                slog("[launch] WARNING: X socket NOT ready after ~5s timeout");
                kstrcpy(g_launch_status, "X SOCKET TIMEOUT - RETRYING SPAWN", 64);
            }
        }
    }

    /* Phase 3: exec_async spawn the app */
    if (path && g_lxc_svc->exec_async) {
        g_launch_phase = 3;
        kstrcpy(g_launch_status, "SPAWNING APPLICATION . . .", 64);
        slog("[launch] phase 3: exec_async spawn");
        slog(path);
        int rc = g_lxc_svc->exec_async(path, argc, argv);
        g_launch_exec_rc = rc;
        slog_num("[launch] exec_async rc=", rc);
        if (rc == 0) {
            slog("[launch] exec_async OK - process spawned");
        } else {
            slog("[launch] exec_async FAILED - process not spawned");
            g_vscode_state = 0;   /* P8.1 Fix: 回到未启动，UI 显示错误而非永远 STARTING */
            g_launch_phase = 7;
            kstrcpy(g_launch_status, "SPAWN FAILED - CHECK SERIAL LOG", 64);
            return;
        }

        /* Phase 4: pgrep verify process exists (bounded wait ~1s) */
        g_launch_phase = 4;
        kstrcpy(g_launch_status, "VERIFYING PROCESS . . .", 64);
        slog("[launch] phase 4: pgrep verify");
        if (g_lxc_svc->exec) {
            /* 用 basename 做 pgrep（code -> pgrep code） */
            const char *pargv[] = {
                "sh", "-c",
                "sleep 0.5; pgrep -x code >/dev/null 2>&1 && echo FOUND || echo NOPE"
            };
            char pout[16] = {0};
            u64 pec = 0;
            int prc = g_lxc_svc->exec("/bin/sh", 3, pargv, pout, 15, 0, &pec);
            if (pout[0] == 'F') {
                g_launch_pgrep_rc = 0;
                slog("[launch] pgrep: VSCode process FOUND");
            } else if (pout[0] == 'N') {
                g_launch_pgrep_rc = 1;
                slog("[launch] pgrep: VSCode process NOT found (may still be starting)");
            } else {
                g_launch_pgrep_rc = -1;
                slog_num("[launch] pgrep: no output, exec rc=", prc);
            }
        }

        /* Phase 5: wait for scanout */
        g_launch_phase = 5;
        kstrcpy(g_launch_status, "WAITING FOR SCANOUT . . .", 64);
        slog("[launch] phase 5: waiting for scanout to come online");
    } else if (path && !g_lxc_svc->exec_async) {
        slog("[launch] exec_async unavailable - cannot spawn app");
        g_launch_phase = 7;
        kstrcpy(g_launch_status, "EXEC_ASYNC UNAVAILABLE", 64);
    }
}

/* VSCode Phase 5/6: 桌面图标启动 Linux 应用（LINUXAPP.CNF 注册项）。
 * 解析 args（空格分隔）后经共享路径 exec_async，并切到 IDE 页 attach。 */
static void launch_linux_app(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return;
    app_descriptor *app = &g_apps[app_id];
    if (!app->linux_path) return;
    if (!g_lxc_svc || !g_lxc_svc->exec_async) {
        slog("linuxapp: lxc service unavailable");
        return;
    }

    /* argv: [name, ...args]（args 在本地缓冲内原地切分） */
    const char *argv[16];
    int argc = 0;
    argv[argc++] = app->name;
    char abuf[96];
    if (app->linux_args && app->linux_args[0]) {
        int n = kstrlen(app->linux_args);
        if (n > (int)sizeof(abuf) - 1) n = (int)sizeof(abuf) - 1;
        for (int i = 0; i < n; i++) abuf[i] = app->linux_args[i];
        abuf[n] = 0;
        char *p = abuf;
        while (*p && argc < 15) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
    }

    guest_gui_launch(app->linux_path, argc, argv);

    /* P7.4: tab 标签跟随最近启动实例（DISPLAY 名，≤15 字符） */
    {
        int i = 0;
        const char *dn = app->display_name;
        while (dn && dn[i] && i + 1 < (int)sizeof(g_ide_tab_label)) {
            g_ide_tab_label[i] = dn[i];
            i++;
        }
        g_ide_tab_label[i] = 0;
    }

    /* 切到 IDE 页并 attach（VSCode 窗口出现在 IDE host 区域） */
    g_ide_attached = 1;
    g_input_forward_enabled = 1;
    ide_attach_invalidate();   /* P7.3: attach 首帧全量重绘 */
    page_switch(2);
    slog("linuxapp launched -> ide attached");
}

/* VSCode Phase 6: IDE attach 共享入口——启动（或复用）VSCode。 */
static void vscode_launch(void) {
    if (g_vscode_app_id >= 0) {
        launch_linux_app(g_vscode_app_id);
        return;
    }
    /* 未注册 VSCode（CNF 缺失）：仅确保 X server 在线，保持 Phase 3 行为 */
    guest_gui_launch(0, 0, 0);
}

/* VSCode Phase 3/7: 把 virtio-gpu scanout 影子缓冲 blit 到 IDE host 区域。
 * scanout 为 1024×768 XRGB8888（stride=4096），与 framebuffer 同为 32bpp。
 * Phase 7 dirty rect 优化：主循环每帧把 scanout 状态缓存进 g_so（查询会
 * 清零 UTSM 侧 dirty），blit 只拷贝累积 dirty rect（scanout 坐标系并集），
 * sprite 双缓冲持久保留未变区域；attach/首帧/状态切换经 g_so_full_repaint
 * 全量重绘一次。 */
static struct linux_compat_scanout_info g_so;
static int g_so_valid = 0;          /* 1 = g_so 为本帧主循环 poll 的缓存 */
static int g_so_full_repaint = 1;   /* 1 = 下次 blit 全量拷贝（初始/attach/上线） */

/* attach/detach 时使 scanout 缓存失效并请求全量重绘 */
static void ide_attach_invalidate(void) {
    g_so_full_repaint = 1;
    g_so_valid = 0;
}

/* 获取 scanout 状态：优先本帧缓存；缓存无效时查询并缓存。 */
static const struct linux_compat_scanout_info *ide_scanout_state(void) {
    if (g_so_valid) return &g_so;
    if (g_lxc_svc && g_lxc_svc->gpu_get_scanout_info &&
        g_lxc_svc->gpu_get_scanout_info(&g_so) == 0) {
        g_so_valid = 1;
        return &g_so;
    }
    return 0;
}

static void blit_scanout_to_ide(int hx, int hy, int hw, int hh) {
    const struct linux_compat_scanout_info *si = ide_scanout_state();
    if (!si || !si->enabled || !si->host_vaddr) {
        /* scanout 未就绪：X server 尚未 SET_SCANOUT，显示等待提示 */
        du_fill_rect(&g_fb, hx, hy, hw, hh, 0xFF051828u);
        draw_centered("WAITING FOR GUEST X SERVER . . .",
                      hx + hw / 2, hy + hh / 2 - 8, KS_TEXT_DIM, 0);
        return;
    }

    int sw = (int)si->width, sh = (int)si->height;
    int sstride = (int)si->stride;   /* bytes/row */
    int dstride = (int)g_fb_pitch;   /* bytes/row */
    u8 *src = (u8 *)si->host_vaddr;
    u8 *dst_base = (u8 *)g_fb.fb;

    /* 拷贝区域：默认仅累积 dirty rect；full repaint 时整帧 */
    int full = g_so_full_repaint;
    int cx = 0, cy = 0, cw = sw, ch = sh;
    if (!full) {
        if (!si->dirty || si->dirty_w == 0 || si->dirty_h == 0) {
            g_prof_blit_skip++;   /* P7.9: dirty=0 零拷贝跳过 */
            return;
        }
        cx = (int)si->dirty_x; cy = (int)si->dirty_y;
        cw = (int)si->dirty_w; ch = (int)si->dirty_h;
    }
    g_so_full_repaint = 0;

    /* P7.9: TSC 计时 blit */
    u64 _tsc0 = rdtsc();
    if (full) g_prof_blit_full++;

    /* clamp 到 host 区域与 scanout 边界 */
    if (cx + cw > hw) cw = hw - cx;
    if (cy + ch > hh) ch = hh - cy;
    if (cw > sw - cx) cw = sw - cx;
    if (ch > sh - cy) ch = sh - cy;
    if (cw <= 0 || ch <= 0) return;

    for (int row = 0; row < ch; row++) {
        u32 *s = (u32 *)(src + (u64)(cy + row) * sstride + (u64)cx * 4);
        u32 *d = (u32 *)(dst_base + (u64)(hy + cy + row) * dstride
                       + (u64)(hx + cx) * 4);
        for (int col = 0; col < cw; col++) d[col] = s[col];
    }

    /* 全量重绘时，scanout 未覆盖的 host 剩余区域填深色背景 */
    if (full) {
        int cov_w = (hw < sw) ? hw : sw;
        int cov_h = (hh < sh) ? hh : sh;
        if (cov_w < hw)
            du_fill_rect(&g_fb, hx + cov_w, hy, hw - cov_w, cov_h, 0xFF051828u);
        if (cov_h < hh)
            du_fill_rect(&g_fb, hx, hy + cov_h, hw, hh - cov_h, 0xFF051828u);
    }

    /* P7.6: 叠加 guest 硬件光标（ARGB alpha-blend 到 framebuffer）。
     * 光标位图 row stride = 64（virtio-gpu 规范 cursor max 64x64）。
     * 坐标从 scanout 坐标系映射到 framebuffer：(hx + cursor_x - hot_x, ...) */
    if (si->cursor_visible && si->cursor_bitmap) {
        int cw = (int)si->cursor_w, ch = (int)si->cursor_h;
        if (cw > 0 && ch > 0) {
            int fx = hx + (int)si->cursor_x - (int)si->cursor_hot_x;
            int fy = hy + (int)si->cursor_y - (int)si->cursor_hot_y;
            u32 *cbmp = (u32 *)si->cursor_bitmap;
            for (int row = 0; row < ch; row++) {
                int dy = fy + row;
                if (dy < 0 || (u64)dy >= g_fb_h) continue;
                u32 *d = (u32 *)((u8 *)g_fb.fb + (u64)dy * (u64)g_fb_pitch);
                const u32 *s = &cbmp[row * 64];   /* row stride = VGPU_CURSOR_MAX */
                for (int col = 0; col < cw; col++) {
                    int dx = fx + col;
                    if (dx < 0 || (u64)dx >= g_fb_w) continue;
                    u32 src = s[col];
                    u32 a = (src >> 24) & 0xFF;
                    if (a == 0) continue;
                    if (a == 255) { d[dx] = src; continue; }
                    u32 dst = d[dx];
                    u32 rr = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * (255 - a)) / 255;
                    u32 gg = (((src >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * (255 - a)) / 255;
                    u32 bb = ( (src        & 0xFF) * a + ( dst        & 0xFF) * (255 - a)) / 255;
                    d[dx] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
                }
            }
        }
    }

    /* P7.9: 记录 blit 耗时 */
    g_prof_blit_total += rdtsc() - _tsc0;
    g_prof_blit_count++;
    if (g_tsc_per_sec)
        g_prof_last_blit_us = (int)((rdtsc() - _tsc0) * 1000000 / g_tsc_per_sec);
}

static void draw_ide(void) {
    int W = (int)g_fb_w;
    int top = KATE_TOPBAR_H;

    /* ide-tabs（36px）：dot + 实例标签 + "+ NEW IDE" + ide-hint（Kate 结构） */
    du_fill_rect(&g_fb, 0, top, W, 36, 0xFF051828u);
    du_divider_h(&g_fb, 0, top + 36, W, KS_BORDER);
    int x = 12;
    int ty = top + (36 - (int)DU_ASCII_LINE_H) / 2 + 1;

    /* .dot（8×8 accent2 发光点） */
    du_fill_rect(&g_fb, x, top + 14, 8, 8, KS_ACCENT2);
    x += 16;

    /* 实例标签（Kate .ide-tab）：P7.4 动态标签 = 最近启动实例 DISPLAY 名 */
    {
        const char *label = g_ide_tab_label;
        int bw = kstrlen(label) * (int)DU_ASCII_STEP + 32;
        if (g_ide_attached) {
            du_fill_rect(&g_fb, x, top + 5, bw, 26, KS_ACCENT);
            du_kate_glow_border(&g_fb, x, top + 5, bw, 26, KS_ACCENT);
        }
        du_rect_outline(&g_fb, x, top + 5, bw, 26,
                        g_ide_attached ? KS_ACCENT : KS_BORDER, 1);
        du_draw_string(&g_fb, label, x + 8, ty,
                       g_ide_attached ? KS_TEXT_INVERT : KS_TEXT_DIM, 0, DU_ASCII_STEP);
        du_draw_string(&g_fb, "x", x + bw - 14, ty, KS_DANGER, 0, DU_ASCII_STEP);
        g_ide_tabx[0] = x + bw - 20; g_ide_tabx[1] = top + 5;
        g_ide_tabx[2] = 20; g_ide_tabx[3] = 26;
        x += bw + 8;
    }

    /* "+ NEW IDE" 按钮（Kate .btn，透明底 + 边框 + accent 字） */
    {
        const char *nb = "+ NEW IDE";
        int bw = kstrlen(nb) * (int)DU_ASCII_STEP + 20;
        g_ide_new_rect[0] = x; g_ide_new_rect[1] = top + 7;
        g_ide_new_rect[2] = bw; g_ide_new_rect[3] = 22;
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= top + 7 && g_mouse_y < top + 29;
        if (hover) du_fill_rect(&g_fb, x, top + 7, bw, 22, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, top + 7, bw, 22, KS_BORDER, 1);
        du_draw_string(&g_fb, nb, x + 10, ty, KS_ACCENT, 0, DU_ASCII_STEP);
        x += bw + 8;
    }

    /* ide-hint */
    du_draw_string(&g_fb, "CTRL+SHIFT+TAB CTX . x DETACH . DBL-CLICK HOST ATTACH",
                   x + 8, ty + 1, KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* page-credit（对应 Kate "由 Deaicup 工作室制作"） */
    du_draw_string(&g_fb, "DEAICUP STUDIO",
                   W - 12 - 14 * (int)DU_ASCII_STEP, ty + 1,
                   KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* ide-host（虚线，margin 8px） */
    int hx = 8, hy = top + 36 + 8;
    int hw = W - 16, hh = (int)g_fb_h - hy - 8;
    g_ide_host[0] = hx; g_ide_host[1] = hy;
    g_ide_host[2] = hw; g_ide_host[3] = hh;
    draw_dashed_rect(hx, hy, hw, hh, KS_BORDER);

    if (g_ide_attached) {
        if (g_input_forward_enabled) {
            /* VSCode Phase 6 状态机：启动中探测 scanout 上线 → 运行中 */
            if (g_vscode_state == 1) {
                const struct linux_compat_scanout_info *si = ide_scanout_state();
                if (si && si->enabled) {
                    g_vscode_state = 2;
                    g_launch_phase = 6;
                    g_so_full_repaint = 1;   /* 上线首帧全量重绘 */
                    /* P8.1: scanout 上线日志 + 耗时 */
                    slog("[launch] phase 6: SCANOUT ONLINE");
                    if (g_tsc_per_sec && g_launch_tsc) {
                        u64 ms = (rdtsc() - g_launch_tsc) * 1000 / g_tsc_per_sec;
                        slog_num("[launch] total launch time ms=", (int)ms);
                    }
                    {
                        char sbuf[48];
                        kstrcpy(sbuf, "scanout: ", 48);
                        u32dec(si->width, sbuf + kstrlen(sbuf));
                        kstrcat(sbuf, "x", 48);
                        u32dec(si->height, sbuf + kstrlen(sbuf));
                        slog(sbuf);
                    }
                }
            }
            if (g_vscode_state == 2) {
                /* 运行中：blit guest 合成帧 */
                blit_scanout_to_ide(hx + 2, hy + 2, hw - 4, hh - 4);
            } else {
                /* P8.1: 启动中--显示分阶段状态 + 详细信息 */
                du_fill_rect(&g_fb, hx + 2, hy + 2, hw - 4, hh - 4, 0xFF051828u);
                draw_centered(g_launch_status[0] ? g_launch_status :
                              "STARTING VSCODE . . .",
                              W / 2, hy + hh / 2 - 20, KS_TEXT_DIM, 0);
                /* 详细信息行（phase/exec_rc/pgrep/耗时） */
                {
                    char dbuf[80];
                    int dp = 0;
                    kstrcpy(dbuf + dp, "PHASE:", 80); dp = kstrlen(dbuf);
                    dp += u32dec((u32)g_launch_phase, dbuf + dp);
                    if (g_launch_exec_rc != 0) {
                        kstrcpy(dbuf + dp, " RC:", 80); dp = kstrlen(dbuf);
                        dp += u32dec((u32)(g_launch_exec_rc < 0 ?
                                    (u32)(-g_launch_exec_rc) : (u32)g_launch_exec_rc),
                                    dbuf + dp);
                    }
                    if (g_launch_pgrep_rc >= 0) {
                        kstrcpy(dbuf + dp, " PGREP:", 80); dp = kstrlen(dbuf);
                        kstrcpy(dbuf + dp, g_launch_pgrep_rc == 0 ? "FOUND" : "MISSING",
                                80);
                        dp = kstrlen(dbuf);
                    }
                    if (g_tsc_per_sec && g_launch_tsc) {
                        u64 ms = (rdtsc() - g_launch_tsc) * 1000 / g_tsc_per_sec;
                        kstrcpy(dbuf + dp, " T:", 80); dp = kstrlen(dbuf);
                        dp += u32dec((u32)ms, dbuf + dp);
                        kstrcpy(dbuf + dp, "ms", 80);
                    }
                    draw_centered(dbuf, W / 2, hy + hh / 2 + 4, KS_TEXT_DIM, 0);
                }
            }
        } else {
            editor_draw_to((editor_state *)ADDR_EDDASH, hx + 2, hy + 2, hw - 4, hh - 4);
        }
    } else {
        /* embed-empty（Kate：未启动 VSCode 提示） */
        draw_centered("VSCODE NOT RUNNING . DOUBLE-CLICK OR + NEW IDE TO START",
                      W / 2, hy + hh / 2 - 8, KS_TEXT_DIM, 0);
    }

    /* P7.9: 性能 profiling 叠加（IDE 页面左下角） */
    if (g_ide_attached && g_input_forward_enabled) {
        char buf[80];
        int p = 0;
        kstrcpy(buf + p, "FPS:", 80 - p); p = kstrlen(buf);
        p += u32dec((u32)g_prof_fps, buf + p);
        kstrcpy(buf + p, " BLT:", 80 - p); p = kstrlen(buf);
        p += u32dec((u32)g_prof_last_blit_us, buf + p);
        kstrcpy(buf + p, "us SKP:", 80 - p); p = kstrlen(buf);
        p += u32dec((u32)g_prof_blit_skip, buf + p);
        kstrcpy(buf + p, " FUL:", 80 - p); p = kstrlen(buf);
        p += u32dec((u32)g_prof_blit_full, buf + p);
        du_draw_string(&g_fb, buf, hx + 6, hy + hh - 16,
                       KS_TEXT_DIM, 0, DU_ASCII_STEP);
    }
}

/* ---- 3·DESKTOP ---- */
static void draw_desktop_page(void) {
    /* 图标网格 */
    draw_desktop_icons();

    /* 窗口（当前工作区，从底到顶） */
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (w->minimized || !w->visible) continue;
        if (w->ws != g_ws_cur) continue;

        draw_window_frame(w);

        if (w->app_id >= 0 && w->app_id < g_app_count &&
            g_apps[w->app_id].on_draw && w->app_state) {
            app_ctx ctx;
            fill_app_ctx(&ctx, w);
            g_apps[w->app_id].on_draw(w->app_state, &ctx);
        }

        du_kate_scanlines(&g_fb, w->x + 1, w->y + KATE_TITLEBAR_H,
                          w->w - 2, w->h - KATE_TITLEBAR_H - 1);
    }

    /* 任务栏（40px） */
    int W = (int)g_fb_w;
    int tb_y = (int)g_fb_h - KATE_TASKBAR_H;
    du_fill_rect_gradient(&g_fb, 0, tb_y, W, KATE_TASKBAR_H,
                          0xFF0A2840u, KS_BG_PRIMARY);
    du_divider_h(&g_fb, 0, tb_y, W, KS_BORDER);

    int x = 8;
    int ty = tb_y + (KATE_TASKBAR_H - (int)DU_ASCII_LINE_H) / 2 + 1;

    /* tb-item per window（当前工作区） */
    for (int i = 0; i < g_win_count; i++) {
        desktop_window *w = &g_windows[i];
        if (!w->visible || w->ws != g_ws_cur) {
            g_tb_rect[i][2] = 0; g_tb_close_rect[i][2] = 0;
            continue;
        }
        char t[16];
        int tl = 0;
        while (w->title[tl] && tl < 11) { t[tl] = w->title[tl]; tl++; }
        if (w->title[tl]) { t[tl-1] = '.'; t[tl] = '.'; tl++; }
        t[tl] = 0;
        int bw = tl * (int)DU_ASCII_STEP + 34;
        if (bw > 150) bw = 150;
        int active = w->focused;
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= tb_y + 6 && g_mouse_y < tb_y + 34;
        if (active) du_fill_rect(&g_fb, x, tb_y + 6, bw, 28, KS_ACCENT_DIM);
        else if (hover) du_fill_rect(&g_fb, x, tb_y + 6, bw, 28, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, tb_y + 6, bw, 28,
                        active ? KS_ACCENT : KS_BORDER, 1);
        du_draw_string(&g_fb, t, x + 6, ty,
                       active ? KS_ACCENT : KS_TEXT_PRIMARY, 0, DU_ASCII_STEP);
        du_draw_string(&g_fb, "x", x + bw - 14, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);
        g_tb_rect[i][0] = x; g_tb_rect[i][1] = tb_y + 6;
        g_tb_rect[i][2] = bw; g_tb_rect[i][3] = 28;
        g_tb_close_rect[i][0] = x + bw - 20; g_tb_close_rect[i][1] = tb_y + 6;
        g_tb_close_rect[i][2] = 20; g_tb_close_rect[i][3] = 28;
        x += bw + 6;
        if (x > W - 260) break;
    }

    /* Winux-Kate 任务栏右组（margin-left:auto）：
     * [+ ADOPT 虚框] [tb-credit 署名] [EXIT 危险色] */
    const char *credit = "DEAICUP STUDIO";
    const char *quit = "EXIT";
    const char *adopt = "+ ADOPT";

    int qw = 60;
    int qx = W - qw - 8;
    int credit_w = kstrlen(credit) * (int)DU_ASCII_STEP;
    int cx = qx - 8 - credit_w;
    int aw = kstrlen(adopt) * (int)DU_ASCII_STEP + 16;
    int ax = cx - 8 - aw;

    /* tb-adopt：虚线 accent 边框 */
    g_tb_adopt_rect[0] = ax; g_tb_adopt_rect[1] = tb_y + 6;
    g_tb_adopt_rect[2] = aw; g_tb_adopt_rect[3] = 28;
    {
        int hover = g_mouse_x >= ax && g_mouse_x < ax + aw &&
                    g_mouse_y >= tb_y + 6 && g_mouse_y < tb_y + 34;
        if (hover) du_fill_rect(&g_fb, ax, tb_y + 6, aw, 28, KS_ACCENT_DIM);
        draw_dashed_rect(ax, tb_y + 6, aw, 28, KS_ACCENT);
        du_draw_string(&g_fb, adopt, ax + 8, ty, KS_ACCENT, 0, DU_ASCII_STEP);
    }

    /* tb-credit */
    du_draw_string(&g_fb, credit, cx, ty, KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* tb-quit */
    g_tb_quit_rect[0] = qx; g_tb_quit_rect[1] = tb_y + 6;
    g_tb_quit_rect[2] = qw; g_tb_quit_rect[3] = 28;
    {
        int hover = g_mouse_x >= qx && g_mouse_x < qx + qw &&
                    g_mouse_y >= tb_y + 6 && g_mouse_y < tb_y + 34;
        if (hover) du_fill_rect(&g_fb, qx, tb_y + 6, qw, 28, 0x30FF4D6Du);
        du_rect_outline(&g_fb, qx, tb_y + 6, qw, 28, KS_DANGER, 1);
        du_draw_string(&g_fb, quit, qx + (qw - 4 * (int)DU_ASCII_STEP) / 2, ty,
                       KS_DANGER, 0, DU_ASCII_STEP);
    }
}

/* ---- 5+/CUSTOM ---- */
static int g_cp_new_rect[4];           /* 自定义页 "+ NEW INSTANCE" 按钮 */

static void draw_custom_page(int cp_idx) {
    int W = (int)g_fb_w;
    int top = KATE_TOPBAR_H;
    cpage *cp = &g_cpages[cp_idx];

    /* ide-tabs 栏（36px）：dot + 页名 + "+ NEW INSTANCE" + ide-hint（Kate 结构） */
    du_fill_rect(&g_fb, 0, top, W, 36, 0xFF051828u);
    du_divider_h(&g_fb, 0, top + 36, W, KS_BORDER);
    int ty = top + (36 - (int)DU_ASCII_LINE_H) / 2 + 1;

    /* .dot + 页名 */
    du_fill_rect(&g_fb, 12, top + 14, 8, 8, KS_ACCENT2);
    du_draw_string(&g_fb, cp->name, 28, ty, KS_ACCENT, 0, DU_ASCII_STEP);
    int x = 28 + kstrlen(cp->name) * (int)DU_ASCII_STEP + 12;

    /* "+ NEW INSTANCE" 按钮（对应 Kate custom launch_custom_new） */
    {
        const char *nb = "+ NEW INSTANCE";
        int bw = kstrlen(nb) * (int)DU_ASCII_STEP + 20;
        g_cp_new_rect[0] = x; g_cp_new_rect[1] = top + 7;
        g_cp_new_rect[2] = bw; g_cp_new_rect[3] = 22;
        int hover = g_mouse_x >= x && g_mouse_x < x + bw &&
                    g_mouse_y >= top + 7 && g_mouse_y < top + 29;
        if (hover) du_fill_rect(&g_fb, x, top + 7, bw, 22, KS_ACCENT_DIM);
        du_rect_outline(&g_fb, x, top + 7, bw, 22, KS_BORDER, 1);
        du_draw_string(&g_fb, nb, x + 10, ty, KS_ACCENT, 0, DU_ASCII_STEP);
        x += bw + 8;
    }

    /* ide-hint */
    du_draw_string(&g_fb, "DBL-CLICK HOST LAUNCH . EXTERNAL ELF FULLSCREEN",
                   x + 8, ty + 1, KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* page-credit */
    du_draw_string(&g_fb, "DEAICUP STUDIO",
                   W - 12 - 14 * (int)DU_ASCII_STEP, ty + 1,
                   KS_TEXT_DIM, 0, DU_ASCII_STEP);

    /* ide-host（虚线，margin 8px） */
    int hx = 8, hy = top + 36 + 8;
    int hw = W - 16, hh = (int)g_fb_h - hy - 8;
    g_cp_host[0] = hx; g_cp_host[1] = hy;
    g_cp_host[2] = hw; g_cp_host[3] = hh;
    draw_dashed_rect(hx, hy, hw, hh, KS_BORDER);

    /* embed-empty（Kate：正在启动提示） */
    char msg[40];
    kstrcpy(msg, "DOUBLE-CLICK TO LAUNCH ", 40);
    kstrcat(msg, cp->name, 40);
    draw_centered(msg, hx + hw / 2, hy + hh / 2 - 12, KS_TEXT_DIM, 0);

    char sub[40];
    kstrcpy(sub, "ELF: ", 40);
    {
        char disp[13];
        f32_name_from_83(cp->elf11, disp);
        kstrcat(sub, disp, 40);
    }
    draw_centered(sub, hx + hw / 2, hy + hh / 2 + 12, KS_ACCENT, 0);
}

/* ============================================================
 *  任务视图 overlay
 * ============================================================ */

static void draw_taskview(void) {
    int W = (int)g_fb_w;

    /* 背景暗化（全屏 blend，模态 overlay 下文字可读性优先） */
    for (u64 ry = 0; ry < g_fb_h; ry++) {
        u32 *line = (u32 *)((u8 *)g_fb.fb + ry * g_fb_pitch);
        for (u64 rx = 0; rx < g_fb_w; rx++) {
            line[rx] = du_blend(line[rx], KS_BG_PRIMARY, 220);
        }
    }

    /* tv-header */
    du_draw_string(&g_fb, "TASK VIEW", 60, 44, KS_ACCENT, 0, DU_ASCII_STEP + 4);
    du_draw_string(&g_fb, "CLICK TO SWITCH . ESC CLOSE . x CLOSE WS",
                   60 + 9 * ((int)DU_ASCII_STEP + 4) + 20, 48,
                   KS_TEXT_DIM, 0, DU_ASCII_STEP);
    du_divider_h(&g_fb, 60, 76, W - 120, KS_BORDER);

    /* tv-grid 卡片 */
    int cw = 260, ch = 170, gap = 20;
    int cols = (W - 120 + gap) / (cw + gap);
    if (cols < 1) cols = 1;
    for (int i = 0; i <= g_ws_count; i++) {
        int row = i / cols, col = i % cols;
        int x = 60 + col * (cw + gap);
        int y = 96 + row * (ch + gap);
        g_tv_card[i][0] = x; g_tv_card[i][1] = y;
        g_tv_card[i][2] = cw; g_tv_card[i][3] = ch;

        if (i == g_ws_count) {
            /* 新建工作区卡片（虚线 +） */
            draw_dashed_rect(x, y, cw, ch, KS_ACCENT2);
            du_draw_string(&g_fb, "+", x + cw / 2 - 6, y + ch / 2 - 24,
                           KS_ACCENT2, 0, DU_ASCII_STEP);
            draw_centered("NEW WORKSPACE", x + cw / 2, y + ch / 2 + 8,
                          KS_ACCENT2, 0);
            continue;
        }

        int active = (i == g_ws_cur);
        int hover = g_mouse_x >= x && g_mouse_x < x + cw &&
                    g_mouse_y >= y && g_mouse_y < y + ch;
        du_fill_rect(&g_fb, x, y, cw, ch, KS_BG_SECONDARY);
        du_rect_outline(&g_fb, x, y, cw, ch,
                        active ? KS_ACCENT2 : (hover ? KS_ACCENT : KS_BORDER), 1);
        if (active) du_kate_glow_border(&g_fb, x, y, cw, ch, KS_ACCENT2);

        /* tv-card-top */
        du_draw_string(&g_fb, g_ws[i].name, x + 12, y + 10, KS_ACCENT, 0, DU_ASCII_STEP);
        du_divider_h(&g_fb, x, y + 34, cw, KS_BORDER);
        /* × close */
        g_tv_close[i][0] = x + cw - 26; g_tv_close[i][1] = y + 4;
        g_tv_close[i][2] = 22; g_tv_close[i][3] = 26;
        du_draw_string(&g_fb, "x", x + cw - 20, y + 8, KS_TEXT_DIM, 0, DU_ASCII_STEP);

        /* tv-stat */
        char nbuf[4];
        int wc = ws_win_count(i);
        nbuf[0] = '0' + (char)(wc % 10); nbuf[1] = 0;
        draw_centered(nbuf, x + cw / 4, y + 70, KS_ACCENT2, 0);
        draw_centered("WINS", x + cw / 4, y + 96, KS_TEXT_DIM, 0);
        char pbuf[6];
        kstrcpy(pbuf, "PG", 6);
        pbuf[2] = '0' + (char)(g_ws[i].page % 10); pbuf[3] = 0;
        draw_centered(pbuf, x + cw * 3 / 4, y + 70, KS_ACCENT2, 0);
        draw_centered("PAGE", x + cw * 3 / 4, y + 96, KS_TEXT_DIM, 0);

        /* badge CURRENT */
        if (active) {
            int bx = x + cw - 84, by = y + 6;
            du_fill_rect(&g_fb, bx, by, 56, 14, KS_ACCENT2);
            du_draw_string(&g_fb, "CURRENT", bx + 2, by - 1,
                           KS_TEXT_INVERT, 0, DU_ASCII_STEP);
        }
    }
}

/* ============================================================
 *  右键菜单（移到工作区）
 * ============================================================ */

/* 构建"移到工作区"列表（排除当前工作区，与 Winux-Kate 一致），返回行数 */
static int ctx_list_build(int *map) {
    int n = 0;
    for (int i = 0; i < g_ws_count; i++) {
        if (i == g_ws_cur) continue;
        map[n++] = i;
    }
    return n;
}

static void draw_ctx_menu(void) {
    if (!g_ctx_open) return;
    int lmap[MAX_WS];
    int ln = ctx_list_build(lmap);
    int rows = (ln ? ln : 1) + 1;        /* 列表行(或空态行) + 新建行 */
    int mw = 200;
    int mh = 24 + rows * 22 + 8;
    int x = g_ctx_x, y = g_ctx_y;
    if (x + mw > (int)g_fb_w) x = (int)g_fb_w - mw - 4;
    if (y + mh > (int)g_fb_h) y = (int)g_fb_h - mh - 4;
    g_ctx_rect[0] = x; g_ctx_rect[1] = y;
    g_ctx_rect[2] = mw; g_ctx_rect[3] = mh;

    du_fill_rect(&g_fb, x, y, mw, mh, 0xFF050A18u);
    du_rect_outline(&g_fb, x, y, mw, mh, KS_ACCENT, 1);
    du_kate_glow_border(&g_fb, x, y, mw, mh, KS_ACCENT);

    du_draw_string(&g_fb, "MOVE TO WORKSPACE", x + 12, y + 5,
                   KS_ACCENT, 0, DU_ASCII_STEP);
    du_divider_h(&g_fb, x, y + 22, mw, KS_BORDER);

    int iy = y + 26;
    int row = 0;
    if (ln == 0) {
        /* 空态（不可点击） */
        du_draw_string(&g_fb, "NO OTHER WORKSPACE", x + 12, iy + 2,
                       KS_TEXT_DIM, 0, DU_ASCII_STEP);
        g_ctx_item[0][2] = 0;            /* 禁用命中 */
        g_ctx_item_ws[0] = -2;
        iy += 22;
        row = 1;
    }
    for (int r = 0; r < ln; r++) {
        int wi = lmap[r];
        int hover = g_mouse_x >= x && g_mouse_x < x + mw &&
                    g_mouse_y >= iy && g_mouse_y < iy + 22;
        if (hover) du_fill_rect(&g_fb, x + 1, iy, mw - 2, 22, KS_ACCENT_DIM);
        du_draw_string(&g_fb, g_ws[wi].name, x + 12, iy + 2,
                       hover ? KS_ACCENT : KS_TEXT_PRIMARY, 0, DU_ASCII_STEP);
        char cn[4];
        cn[0] = '0' + (char)(ws_win_count(wi) % 10); cn[1] = 0;
        du_draw_string(&g_fb, cn, x + mw - 24, iy + 2, KS_TEXT_DIM, 0, DU_ASCII_STEP);
        g_ctx_item[row][0] = x; g_ctx_item[row][1] = iy;
        g_ctx_item[row][2] = mw; g_ctx_item[row][3] = 22;
        g_ctx_item_ws[row] = wi;
        iy += 22;
        row++;
    }
    /* + NEW WORKSPACE */
    du_divider_h(&g_fb, x, iy, mw, KS_BORDER);
    int hover = g_mouse_x >= x && g_mouse_x < x + mw &&
                g_mouse_y >= iy && g_mouse_y < iy + 22;
    if (hover) du_fill_rect(&g_fb, x + 1, iy, mw - 2, 22, KS_ACCENT_DIM);
    du_draw_string(&g_fb, "+ NEW WORKSPACE", x + 12, iy + 2,
                   KS_ACCENT2, 0, DU_ASCII_STEP);
    g_ctx_item[row][0] = x; g_ctx_item[row][1] = iy;
    g_ctx_item[row][2] = mw; g_ctx_item[row][3] = 22;
    g_ctx_item_ws[row] = -1;
    /* 清除残余行命中 */
    for (int r = row + 1; r <= MAX_WS; r++) g_ctx_item[r][2] = 0;
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

static void redraw_all(void) {
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

/* ============================================================
 *  工作区 / 页面管理
 * ============================================================ */

static void ws_switch(int id) {
    if (id < 0 || id >= g_ws_count || id == g_ws_cur) { g_taskview = 0; return; }
    g_ws[g_ws_cur].page = g_page;      /* 记住离开时的页面 */
    g_ws_cur = id;
    g_page = g_ws[id].page;
    g_taskview = 0;
}

static void ws_create(void) {
    if (g_ws_count >= MAX_WS) return;
    char *nm = g_ws[g_ws_count].name;
    nm[0] = 'W'; nm[1] = 'S'; nm[2] = '-';
    nm[3] = '0' + (char)(g_ws_count + 1); nm[4] = 0;
    g_ws[g_ws_count].page = 1;
    g_ws_count++;
}

/* 关闭工作区：窗口释放到关闭后的当前工作区（Kate release 语义） */
static void ws_close(int id) {
    if (g_ws_count <= 1 || id < 0 || id >= g_ws_count) return;
    int cur_after = g_ws_cur;
    if (g_ws_cur == id) cur_after = (id > 0) ? id - 1 : 0;
    else if (g_ws_cur > id) cur_after = g_ws_cur - 1;
    for (int i = 0; i < g_win_count; i++) {
        if (g_windows[i].ws == id) g_windows[i].ws = cur_after;
        else if (g_windows[i].ws > id) g_windows[i].ws--;
    }
    for (int i = id; i < g_ws_count - 1; i++) g_ws[i] = g_ws[i + 1];
    g_ws_count--;
    g_ws_cur = cur_after;
    g_taskview = 0;
}

static void page_switch(int p) {
    if (p < 1 || p > 3 + g_cpage_count) return;
    g_page = p;
}

/* 自定义页：为尚未挂载的外部 .elf 应用新建页面 */
static void cpage_add(void) {
    if (g_cpage_count >= MAX_CPAGES) return;
    for (int a = 0; a < g_app_count; a++) {
        if (!g_apps[a].elf_name) continue;
        int dup = 0;
        for (int i = 0; i < g_cpage_count; i++)
            if (f32_neq11(g_cpages[i].elf11, g_apps[a].elf_name)) { dup = 1; break; }
        if (dup) continue;
        cpage *cp = &g_cpages[g_cpage_count];
        int n = kstrlen(g_apps[a].display_name);
        if (n > 11) n = 11;
        for (int i = 0; i < n; i++) cp->name[i] = g_apps[a].display_name[i];
        cp->name[n] = 0;
        for (int i = 0; i < 11; i++) cp->elf11[i] = g_apps[a].elf_name[i];
        cp->id = 4 + g_cpage_count;
        g_cpage_count++;
        g_page = cp->id;
        return;
    }
}

static void cpage_remove(int idx) {
    if (idx < 0 || idx >= g_cpage_count) return;
    int removed_page = g_cpages[idx].id;
    for (int i = idx; i < g_cpage_count - 1; i++) g_cpages[i] = g_cpages[i + 1];
    g_cpage_count--;
    for (int i = 0; i < g_cpage_count; i++) g_cpages[i].id = 4 + i;
    if (g_page == removed_page) g_page = 1;
    else if (g_page > removed_page) g_page--;
}

/* ============================================================
 *  事件处理
 * ============================================================ */

static int rect_hit(const int r[4], int mx, int my) {
    return r[2] > 0 && mx >= r[0] && mx < r[0] + r[2] &&
           my >= r[1] && my < r[1] + r[3];
}

static void slider_set(const int rect[4], int *val, int mx) {
    int v = (mx - rect[0]) * 100 / rect[2];
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    *val = v;
}

/* 双击判定（TSC 半秒 + 8px 容差），不更新状态 */
static int dbl_check(int mx, int my) {
    if (!g_tsc_per_sec) return 0;
    u64 now = rdtsc();
    int dx = mx - g_last_click_x, dy = my - g_last_click_y;
    return (now - g_last_click_tsc) < g_tsc_per_sec / 2 &&
           dx > -8 && dx < 8 && dy > -8 && dy < 8;
}

/* ---- 1·DASHBOARD 点击 ---- */
static int dash_click(int mx, int my, int dbl) {
    editor_state *ed = (editor_state *)ADDR_EDDASH;

    /* SAVE 按钮 */
    if (ed->has_file && rect_hit(g_save_rect, mx, my)) { editor_save(); return 1; }

    /* 面板聚焦 */
    for (int i = 0; i < 4; i++) {
        if (!rect_hit(g_dash_rect[i], mx, my)) continue;
        g_dash_focus = i;
        if (i == 3) {
            /* FILES 行命中 / 双击打开 */
            int fx = g_dash_rect[3][0] + 2;
            int fy = g_dash_rect[3][1] + KATE_PANEL_HEADER_H + 2;
            int fw = g_dash_rect[3][2] - 4;
            int fh = g_dash_rect[3][3] - KATE_PANEL_HEADER_H - 4;
            if (mx >= fx && mx < fx + fw && my >= fy + 24) {
                int max_rows = (fh - 24) / 22;
                int row = (my - fy - 24) / 22;
                if (row >= 0 && row < max_rows && row < g_file_count) {
                    if (dbl && row == g_file_sel) files_open(row);
                    else g_file_sel = row;
                }
            }
        }
        return 1;
    }

    /* 状态栏滑块 */
    if (rect_hit(g_vol_rect, mx, my)) {
        g_drag_slider = 1;
        slider_set(g_vol_rect, &g_vol, mx);
        return 1;
    }
    if (rect_hit(g_bri_rect, mx, my)) {
        g_drag_slider = 2;
        slider_set(g_bri_rect, &g_bright, mx);
        return 1;
    }
    return 0;
}

/* ---- 2·IDE 点击 ---- */
static int ide_click(int mx, int my, int dbl) {
    if (g_ide_attached && rect_hit(g_ide_tabx, mx, my)) {
        g_ide_attached = 0;
        /* VSCode Phase 2: detach 时停止向 guest 转发输入 */
        g_input_forward_enabled = 0;
        ide_attach_invalidate();   /* P7.3: detach 后下次 attach 全量重绘 */
        slog("ide detached, input forwarding off");
        return 1;
    }
    if (!g_ide_attached && rect_hit(g_ide_new_rect, mx, my)) {
        g_ide_attached = 1;
        ide_attach_invalidate();   /* P7.3: attach 首帧全量重绘 */
        /* attach 时仅当 Linux guest 服务可用才开启转发；否则降级为本地 editor */
        g_input_forward_enabled = (g_lxc_svc != 0);
        slog(g_input_forward_enabled ? "ide attached, forwarding to guest"
                                    : "ide attached, lxc unavailable (local editor)");
        /* VSCode Phase 6: 共享启动路径（X server + VSCode） */
        if (g_input_forward_enabled) vscode_launch();
        return 1;
    }
    if (!g_ide_attached && dbl && rect_hit(g_ide_host, mx, my)) {
        g_ide_attached = 1;
        ide_attach_invalidate();   /* P7.3: attach 首帧全量重绘 */
        g_input_forward_enabled = (g_lxc_svc != 0);
        slog(g_input_forward_enabled ? "ide attached (dbl), forwarding to guest"
                                    : "ide attached (dbl), lxc unavailable (local editor)");
        /* VSCode Phase 6: 共享启动路径（X server + VSCode） */
        if (g_input_forward_enabled) vscode_launch();
        return 1;
    }
    return 0;
}

/* ---- 4·DESKTOP 点击 ---- */
static int desktop_click(int mx, int my, int dbl) {
    int tb_y = (int)g_fb_h - KATE_TASKBAR_H;

    /* 任务栏 */
    if (my >= tb_y) {
        if (rect_hit(g_tb_quit_rect, mx, my)) { g_quit = 1; return 1; }
        if (rect_hit(g_tb_adopt_rect, mx, my)) {
            /* 收纳窗口：全部工作区窗口收归当前工作区并还原最小化 */
            for (int i = 0; i < g_win_count; i++) {
                g_windows[i].ws = g_ws_cur;
                g_windows[i].minimized = 0;
            }
            return 1;
        }
        for (int i = 0; i < g_win_count; i++) {
            if (!g_tb_rect[i][2] || !rect_hit(g_tb_rect[i], mx, my)) continue;
            if (rect_hit(g_tb_close_rect[i], mx, my)) {
                win_close_request(&g_windows[i]);
            } else {
                win_focus(&g_windows[i]);
            }
            return 1;
        }
        return 0;
    }

    /* 窗口 */
    desktop_window *w = win_find_at(mx, my);
    if (w) {
        int wid = w->id;
        win_focus(w);
        w = win_find(wid);   /* focus 重排后重新定位 */
        if (!w) return 1;
        if (win_hit_close_btn(w, mx, my)) { win_close_request(w); return 1; }
        if (win_hit_titlebar(w, mx, my)) {
            g_dragging = 1;
            g_drag_win = w->id;
            g_drag_off_x = mx - w->x;
            g_drag_off_y = my - w->y;
            return 1;
        }
        /* 客户区事件 → 应用 */
        if (w->app_id >= 0 && w->app_id < g_app_count &&
            g_apps[w->app_id].on_event && w->app_state) {
            desktop_event dev;
            dev.type = EV_MOUSE_DOWN;
            dev.mx = mx; dev.my = my;
            dev.button = 0; dev.scancode = 0;
            dev.shift = g_shift; dev.ctrl = g_ctrl; dev.alt = g_alt;
            app_ctx actx;
            fill_app_ctx(&actx, w);
            g_apps[w->app_id].on_event(w->app_state, &actx, &dev);
        }
        return 1;
    }

    /* 图标（单击选中 / 双击启动） */
    for (int i = 0; i < g_icon_count; i++) {
        int ix = g_icons[i].x, iy = g_icons[i].y;
        if (mx >= ix && mx < ix + ICON_W && my >= iy && my < iy + ICON_H) {
            if (dbl && g_icons[i].selected) {
                launch_app(g_icons[i].app_id);
            } else {
                for (int j = 0; j < g_icon_count; j++) g_icons[j].selected = 0;
                g_icons[i].selected = 1;
            }
            return 1;
        }
    }

    /* 空白：取消选择 */
    for (int j = 0; j < g_icon_count; j++) g_icons[j].selected = 0;
    return 1;
}

/* ---- 左键按下 ---- */
static int on_left_press(void) {
    int mx = g_mouse_x, my = g_mouse_y;
    int dbl = dbl_check(mx, my);
    g_last_click_tsc = rdtsc();
    g_last_click_x = mx; g_last_click_y = my;

    /* 任务视图 overlay（模态） */
    if (g_taskview) {
        for (int i = 0; i <= g_ws_count; i++) {
            if (!rect_hit(g_tv_card[i], mx, my)) continue;
            if (i == g_ws_count) ws_create();
            else if (rect_hit(g_tv_close[i], mx, my)) ws_close(i);
            else ws_switch(i);
            return 1;
        }
        g_taskview = 0;   /* 背景点击关闭 */
        return 1;
    }

    /* 右键菜单（模态） */
    if (g_ctx_open) {
        if (rect_hit(g_ctx_rect, mx, my)) {
            for (int i = 0; i <= MAX_WS; i++) {
                if (!rect_hit(g_ctx_item[i], mx, my)) continue;
                int wi = g_ctx_item_ws[i];
                if (wi == -1) {
                    /* + NEW WORKSPACE：新建并迁移 */
                    ws_create();
                    desktop_window *w = win_find(g_ctx_win);
                    if (w && g_ws_count > 0) w->ws = g_ws_count - 1;
                } else if (wi >= 0) {
                    desktop_window *w = win_find(g_ctx_win);
                    if (w) w->ws = wi;
                }
                break;
            }
        }
        g_ctx_open = 0;
        return 1;
    }

    /* 顶栏 */
    if (my < KATE_TOPBAR_H) {
        if (rect_hit(g_tvbtn_rect, mx, my)) { g_taskview = 1; return 1; }
        for (int i = 0; i < g_ws_count; i++)
            if (rect_hit(g_ws_rect[i], mx, my)) { ws_switch(i); return 1; }
        if (rect_hit(g_wsadd_rect, mx, my)) { ws_create(); return 1; }
        for (int i = 0; i < 3 + g_cpage_count; i++) {
            if (i >= 3 && rect_hit(g_pg_rm_rect[i], mx, my)) {
                cpage_remove(i - 3);
                return 1;
            }
            if (rect_hit(g_pg_rect[i], mx, my)) { page_switch(i + 1); return 1; }
        }
        if (rect_hit(g_pgadd_rect, mx, my)) { cpage_add(); return 1; }
        return 0;
    }

    /* 页面内容 */
    if (g_page == 1) return dash_click(mx, my, dbl);
    if (g_page == 2) return ide_click(mx, my, dbl);
    if (g_page == 3) return desktop_click(mx, my, dbl);
    if (g_page >= 4) {
        int launch = 0;
        if (rect_hit(g_cp_new_rect, mx, my)) launch = 1;
        if (dbl && rect_hit(g_cp_host, mx, my)) launch = 1;
        if (launch) {
            for (int i = 0; i < g_cpage_count; i++) {
                if (g_cpages[i].id != g_page) continue;
                launch_external_elf(g_cpages[i].elf11);
                break;
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- 右键按下：窗口/任务栏项 → 移到工作区菜单 ---- */
static int on_right_press(void) {
    int mx = g_mouse_x, my = g_mouse_y;

    if (g_taskview) { g_taskview = 0; return 1; }
    if (g_ctx_open) { g_ctx_open = 0; return 1; }
    if (g_page != 3) return 0;

    int tb_y = (int)g_fb_h - KATE_TASKBAR_H;
    if (my >= tb_y) {
        for (int i = 0; i < g_win_count; i++) {
            if (!g_tb_rect[i][2] || !rect_hit(g_tb_rect[i], mx, my)) continue;
            int lmap[MAX_WS];
            int ln = ctx_list_build(lmap);
            int mh = 24 + ((ln ? ln : 1) + 1) * 22 + 8;
            g_ctx_win = g_windows[i].id;
            g_ctx_x = mx;
            g_ctx_y = my - mh;  /* 菜单向上展开 */
            if (g_ctx_y < KATE_TOPBAR_H) g_ctx_y = my;
            g_ctx_open = 1;
            return 1;
        }
        return 0;
    }

    desktop_window *w = win_find_at(mx, my);
    if (w) {
        g_ctx_win = w->id;
        g_ctx_x = mx; g_ctx_y = my;
        g_ctx_open = 1;
        return 1;
    }
    return 0;
}

/* ---- 键盘 ---- */
static int handle_key(u8 sc) {
    /* VSCode Phase 2: IDE attached 且在 IDE 页时，把按键序列转发到 Linux guest。
     * 0xE0 前缀字节本身不转发（无对应 keycode），仅靠 g_e0 标志让下一字节选取扩展键变体。
     * 转发在修饰键跟踪与页面路由之前完成，保证 VSCode 收到完整 press/release
     * 序列（含 Shift/Ctrl/Alt/方向键）。宿主修饰键跟踪继续运行，Ctrl+Tab 切页、
     * Ctrl+S 等热键仍可在宿主侧生效，便于用户随时切离 IDE 页。 */
    if (g_input_forward_enabled && g_lxc_svc && g_lxc_svc->input_forward_keyboard &&
        sc != 0xE0 && g_page == 2) {
        u8 base = (u8)(sc & 0x7F);
        u16 code = scancode_to_linux_keycode(base, g_e0);
        if (code) {
            u32 value = (sc & 0x80) ? 0 : 1;   /* 0x80 位 = release */
            g_lxc_svc->input_forward_keyboard(code, value);
        }
    }

    if (sc == 0xE0) { g_e0 = 1; return 0; }
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return 0; }
    if (sc == 0x1D) { g_ctrl = 1; g_e0 = 0; return 0; }
    if (sc == 0x9D) { g_ctrl = 0; return 0; }
    if (sc == 0x38) { g_alt = 1; g_e0 = 0; return 0; }
    if (sc == 0xB8) { g_alt = 0; return 0; }
    if (sc & 0x80) { g_e0 = 0; return 0; }   /* 其他释放码忽略 */

    /* Esc：关闭 overlay */
    if (sc == 0x01) {
        if (g_taskview) { g_taskview = 0; return 1; }
        if (g_ctx_open) { g_ctx_open = 0; return 1; }
        return 0;
    }
    if (g_taskview || g_ctx_open) return 0;   /* 模态期间吞掉按键 */

    /* Ctrl+Tab 切页 / Ctrl+Shift+Tab 上下文菜单（DESKTOP 页） */
    if (g_ctrl && sc == 0x0F) {
        if (g_shift) {
            /* Ctrl+Shift+Tab：对当前聚焦窗口打开"移到工作区"菜单 */
            if (g_page == 3 && win_find(g_focused_win)) {
                g_ctx_win = g_focused_win;
                g_ctx_x = g_mouse_x; g_ctx_y = g_mouse_y;
                g_ctx_open = 1;
                return 1;
            }
            return 0;
        }
        int maxp = 3 + g_cpage_count;
        int p = g_page + 1;
        if (p > maxp) p = 1;
        page_switch(p);
        return 1;
    }
    if (g_ctrl && sc == 0x1F) { editor_save(); return 1; }

    /* 页面路由 */
    if (g_page == 1) {
        if (g_dash_focus <= 1) {
            bash_key((bash_state *)(g_dash_focus == 0 ? ADDR_TERM1 : ADDR_TERM2),
                     sc, g_shift);
            return 1;
        }
        if (g_dash_focus == 2) {
            editor_key((editor_state *)ADDR_EDDASH, sc, g_shift);
            return 1;
        }
        /* FILES 面板导航 */
        if (sc == 0x48) { if (g_file_sel > 0) g_file_sel--; return 1; }
        if (sc == 0x50) { if (g_file_sel < g_file_count - 1) g_file_sel++; return 1; }
        if (sc == 0x1C) { files_open(g_file_sel); return 1; }
        return 0;
    }
    if (g_page == 2) {
        if (g_ide_attached) {
            if (g_input_forward_enabled) {
                /* VSCode Phase 2: 键盘已转发到 guest 中的 VSCode，
                 * 本地 editor 不再重复处理，避免双输入。 */
                return 1;
            }
            editor_key((editor_state *)ADDR_EDDASH, sc, g_shift);
            return 1;
        }
        return 0;
    }
    if (g_page == 3) {
        desktop_window *fw = win_find(g_focused_win);
        if (fw && fw->ws == g_ws_cur && fw->app_id >= 0 && fw->app_id < g_app_count &&
            g_apps[fw->app_id].on_event && fw->app_state) {
            desktop_event dev;
            dev.type = EV_KEY_DOWN;
            dev.mx = g_mouse_x; dev.my = g_mouse_y;
            dev.button = 0;
            dev.scancode = sc;
            dev.shift = g_shift; dev.ctrl = g_ctrl; dev.alt = g_alt;
            app_ctx actx;
            fill_app_ctx(&actx, fw);
            g_apps[fw->app_id].on_event(fw->app_state, &actx, &dev);
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 *  P5d: PE 窗口运行时（run_windowed 桌面嵌入）
 *
 *  协作模型（契约见 utsm/pe.h pe_window_host）：
 *    launch_pe_app 创建受管窗口并把客户区大小的内存 surface 交给 shim，
 *    随后阻塞在 pe_service.run_windowed。PE 消息空转（GetMessage/
 *    PeekMessage 无消息）时 shim 回调 pe_pump_cb，由它驱动 desktop 一帧：
 *    采集 PS/2 输入（PE 窗口聚焦时经 inject_* 注入，否则走桌面正常路径）、
 *    redraw_all + flip。关闭 = inject WM_CLOSE（win_close_request），
 *    PE 自愿退出后 run_windowed 返回，launch_pe_app 收尾销毁窗口。
 *
 *  注意：pump 运行在 PE 的 1MB 大栈上，desktop 帧代码栈用量远小于此；
 *  pump 内禁止再次调用 run/run_windowed（launch_pe_app 以 g_pe_running 拒绝）。
 * ============================================================ */

/* PE 窗口 on_draw：把 shim 渲染的 surface blit 到客户区（32bpp 行拷贝） */
static void pe_win_on_draw(void *state, app_ctx *ctx) {
    (void)state;
    if (!g_pe_surf) return;
    int w = (g_pe_surf_w < ctx->client_w) ? g_pe_surf_w : ctx->client_w;
    int h = (g_pe_surf_h < ctx->client_h) ? g_pe_surf_h : ctx->client_h;
    if (w <= 0 || h <= 0) return;
    u8 *dst0 = (u8 *)g_fb.fb + (u64)ctx->client_y * g_fb_pitch
             + (u64)ctx->client_x * 4;
    u32 sp = (u32)g_pe_surf_w * 4;
    for (int r = 0; r < h; r++) {
        u32 *d = (u32 *)(dst0 + (u64)r * g_fb_pitch);
        const u32 *s = (const u32 *)(g_pe_surf + (u64)r * sp);
        for (int c = 0; c < w; c++) d[c] = s[c] | 0xFF000000u;
    }
}

/* PE 消息空转让出点：驱动 desktop 一帧。返回 0 继续，非 0 请求 PE 退出。 */
static int pe_pump_cb(void *ud) {
    int win_id = (int)(u64)ud;
    int moved = 0;
    while (ps2_mouse_poll()) moved = 1;

    desktop_window *pw = win_find(win_id);
    if (!pw) return 1;                        /* 窗口已销毁：请求 PE 退出 */

    int pe_focused = (g_page == 3 && pw->focused && pw->ws == g_ws_cur &&
                      !g_taskview && !g_ctx_open);

    /* 鼠标注入：PE 窗口聚焦且光标在客户区内 → surface 局部坐标 + 按钮位 */
    int cx0 = pw->x + BORDER_W, cy0 = pw->y + KATE_TITLEBAR_H;
    int ccw = pw->w - 2 * BORDER_W, cch = pw->h - KATE_TITLEBAR_H - BORDER_W;
    if (pe_focused && g_pe_svc->inject_pointer &&
        g_mouse_x >= cx0 && g_mouse_x < cx0 + ccw &&
        g_mouse_y >= cy0 && g_mouse_y < cy0 + cch) {
        g_pe_svc->inject_pointer(g_mouse_x - cx0, g_mouse_y - cy0,
                                 (u32)(g_mouse_btn & 3));
    }

    /* 点击/释放走桌面正常路径（聚焦、标题栏拖拽、关闭按钮→win_close_request、
     * 其他窗口/图标）。双击其他 PE 图标会被 launch_pe_app 的 g_pe_running 拒绝。 */
    if (g_left_pressed)  { g_left_pressed = 0;  on_left_press(); }
    if (g_right_pressed) { g_right_pressed = 0; on_right_press(); }
    if (g_left_released) {
        g_left_released = 0;
        if (g_dragging || g_drag_slider) {
            g_dragging = 0; g_drag_win = -1; g_drag_slider = 0;
        }
    }

    pw = win_find(win_id);   /* 点击可能重排窗口数组 */
    if (!pw) return 1;
    pe_focused = (g_page == 3 && pw->focused && pw->ws == g_ws_cur &&
                  !g_taskview && !g_ctx_open);

    /* 窗口/滑块拖拽（与主循环相同的钳位） */
    if (moved && g_dragging && g_drag_win == win_id) {
        pw->x = g_mouse_x - g_drag_off_x;
        pw->y = g_mouse_y - g_drag_off_y;
        if (pw->y < KATE_TOPBAR_H) pw->y = KATE_TOPBAR_H;
        if (pw->x < 0 - pw->w + 40) pw->x = 0 - pw->w + 40;
        if (pw->x > (int)g_fb_w - 40) pw->x = (int)g_fb_w - 40;
        if (pw->y > (int)g_fb_h - KATE_TASKBAR_H - 20)
            pw->y = (int)g_fb_h - KATE_TASKBAR_H - 20;
    }
    if (moved && g_drag_slider == 1) slider_set(g_vol_rect, &g_vol, g_mouse_x);
    if (moved && g_drag_slider == 2) slider_set(g_bri_rect, &g_bright, g_mouse_x);

    /* 键盘：PE 聚焦时注入（shim 内部合成 KEYUP/CHAR/修饰键），
     * Ctrl+Tab 保留给宿主切页；否则走桌面正常路径。 */
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1) || (st & 0x20)) break;
        u8 sc = inb(0x60);
        if (pe_focused && g_pe_svc->inject_scancode) {
            if (sc == 0xE0) { g_e0 = 1; continue; }
            /* 宿主侧同步跟踪修饰键（Ctrl+Tab 切页判定用） */
            if (sc == 0x2A || sc == 0x36) g_shift = 1;
            if (sc == 0xAA || sc == 0xB6) g_shift = 0;
            if (sc == 0x1D) g_ctrl = 1;
            if (sc == 0x9D) g_ctrl = 0;
            if (g_ctrl && sc == 0x0F) {
                /* 切页前补修饰键释放，防止 PE 侧卡键 */
                g_pe_svc->inject_scancode(0x9D, 0);
                g_pe_svc->inject_scancode(0xAA, 0);
                g_pe_svc->inject_scancode(0xB8, 0);
                g_e0 = 0;
                handle_key(sc);
                continue;
            }
            g_pe_svc->inject_scancode(sc, g_e0);
            g_e0 = 0;
        } else {
            handle_key(sc);
        }
    }

    if (g_quit) return 1;                     /* 任务栏 QUIT：请求 PE 退出 */

    /* 驱动一帧（redraw_all 内含 PE surface blit 与光标绘制） */
    redraw_all();
    flip_buffer();
    return 0;
}

/* 启动 PE 应用：读 exe → 建窗口 → run_windowed 阻塞运行 → 退出后销毁窗口 */
static void launch_pe_app(int app_id) {
    app_descriptor *app = &g_apps[app_id];
    if (!g_pe_svc || !g_pe_svc->run_windowed) {
        slog("pe: service unavailable");
        return;
    }
    if (g_pe_running) {
        slog("pe: another PE window is running");
        return;
    }

    u8 *data = 0; u32 size = 0;
    if (f32_read_path_lfn(app->pe_file, &data, &size) != 0) {
        slog("pe: exe not found");
        return;
    }

    /* 窗口布局：与 launch_app 相同的级联策略 */
    int top = KATE_TOPBAR_H, bot = (int)g_fb_h - KATE_TASKBAR_H;
    int wx = ICON_PAD + g_win_count * 30;
    int wy = top + 8 + g_win_count * 30;
    if (wx + (int)app->default_w > (int)g_fb_w - 20) wx = ICON_PAD;
    if (wy + (int)app->default_h > bot - 20) wy = top + 8;

    desktop_window *win = win_create(app_id, app->display_name,
                                     wx, wy, (int)app->default_w, (int)app->default_h);
    if (!win) return;
    int win_id = win->id;

    int cw = (int)app->default_w - 2 * BORDER_W;
    int ch = (int)app->default_h - KATE_TITLEBAR_H - BORDER_W;
    if (cw <= 0 || ch <= 0 || (u64)cw * (u64)ch * 4 > PE_SURFACE_MAX) {
        win_destroy(win);
        slog("pe: client area too large for surface");
        return;
    }
    g_pe_surf = (u8 *)PE_SURFACE_ADDR;
    g_pe_surf_w = cw; g_pe_surf_h = ch;
    /* 初始化为深色底：PE 窗口未覆盖区域不残留脏数据 */
    {
        u32 *p = (u32 *)g_pe_surf;
        u64 n = (u64)cw * (u64)ch;
        for (u64 i = 0; i < n; i++) p[i] = 0xFF101418u;
    }

    win->app_state = &g_pe_state_dummy;   /* draw_desktop_page 要求非空才调 on_draw */
    g_pe_win_id = win_id;
    g_pe_running = 1;
    slog("pe: run_windowed");

    /* cmdline："file.exe args"（GetCommandLineA 返回值） */
    static char cmdline[160];
    kstrcpy(cmdline, app->pe_file, (int)sizeof(cmdline));
    if (app->pe_args && app->pe_args[0]) {
        kstrcat(cmdline, " ", (int)sizeof(cmdline));
        kstrcat(cmdline, app->pe_args, (int)sizeof(cmdline));
    }

    pe_window_host host;
    host.pump = pe_pump_cb;
    host.ud = (void *)(u64)win_id;        /* 传 id：窗口数组重排不悬空 */
    host.surface = g_pe_surf;
    host.width = (u32)cw;
    host.height = (u32)ch;
    host.pitch = (u32)cw * 4;

    u64 exit_code = 0;
    g_pe_svc->run_windowed(data, size, cmdline, &exit_code, &host);

    g_pe_running = 0;
    g_pe_win_id = -1;
    g_pe_surf = 0;

    /* PE 已退出：销毁宿主窗口并全屏重绘 */
    desktop_window *w2 = win_find(win_id);
    if (w2) win_destroy(w2);
    files_refresh();                      /* PE 可能改动了磁盘 */
    g_full_redraw = 1;
    redraw_all();
    flip_buffer();
    slog("pe: exited");
}

/* ============================================================
 *  主入口
 * ============================================================ */

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    slog("boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        slog("bad context");
        for (;;) __asm__("hlt");
    }
    g_boot_ctx = ctx;

    g_fb_addr = ctx->framebuffer_address;
    g_fb_w = ctx->framebuffer_width;
    g_fb_h = ctx->framebuffer_height;
    g_fb_pitch = ctx->framebuffer_pitch;
    g_kernel_api = (const void *)ctx->dkm_kernel_api;

    /* block API：kernel_api + 0xA8 → block_api 结构，read @ +0x10, write @ +0x18 */
    {
        u64 blk = *(u64 *)(ctx->dkm_kernel_api + 0xA8);
        g_block_read  = blk ? (desktop_block_read_fn)*(u64 *)(blk + 16) : 0;
        g_block_write = blk ? (desktop_block_write_fn)*(u64 *)(blk + 24) : 0;
    }

    /* VSCode Phase 2: Linux 兼容层服务指针（dsk_loader 在 reserved[5] 写入）。
     * 为 NULL 表示 Linux guest 未启动 / IPC 未就绪，IDE attach 时降级为本地 editor。 */
    g_lxc_svc = (const linux_compat_service *)ctx->reserved[5];
    if (g_lxc_svc && g_lxc_svc->magic == LINUX_COMPAT_MAGIC) {
        slog("lxc service available");
    } else {
        g_lxc_svc = 0;
        slog("lxc service unavailable (IDE will use local editor)");
    }

    /* P5d: PE 兼容层服务指针（dsk_loader 在 reserved[4] 写入）。
     * 为 NULL 表示 PE 服务未注册，PEAPPS.CNF 应用启动时降级报错。 */
    g_pe_svc = (const pe_service *)ctx->reserved[4];
    if (g_pe_svc && g_pe_svc->magic == PE_SERVICE_MAGIC) {
        slog("pe service available");
    } else {
        g_pe_svc = 0;
        slog("pe service unavailable");
    }

    du_context_init(&g_fb, g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch);
    /* 双缓冲：保存真实 fb，切换到 sprite buffer 渲染 */
    g_real_fb = (u8 *)g_fb_addr;
    g_fb.fb = (du_u32 *)SPRITE_BUF_ADDR;
    f32_init(g_block_read, g_block_write);

    tsc_calibrate();
    g_rtc_boot_sec = rtc_day_sec();
    g_last_sec = g_rtc_boot_sec;

    ps2_mouse_init();

    register_apps();
    setup_desktop_icons();

    /* 工作区 0 */
    kstrcpy(g_ws[0].name, "WS-1", 16);
    g_ws[0].page = 1;
    g_ws_count = 1;
    g_ws_cur = 0;

    /* DASHBOARD 面板共享状态 */
    {
        bash_state *t1 = (bash_state *)ADDR_TERM1;
        bash_reset(t1, 640, 280);
        bash_puts(t1, "Deshab Bash v0.1 - TERM-01\nType help for commands\n\n", KS_TEXT_PRIMARY);

        bash_state *t2 = (bash_state *)ADDR_TERM2;
        bash_reset(t2, 640, 280);
        bash_puts(t2, "Deshab Bash v0.1 - TERM-02\nType help for commands\n\n", KS_TEXT_PRIMARY);

        editor_reset((editor_state *)ADDR_EDDASH,
            "Welcome to Deshab Editor\n"
            "\n"
            "- Double-click a file in FILES panel to open it here\n"
            "- SAVE button / Ctrl+S writes back to FAT32 disk\n"
            "- IDE page hosts this same editor full-screen\n");
    }

    files_refresh();

    boot_screen();
    g_booted = 1;

    redraw_all();
    slog("desktop ready");

    /* 主事件循环 */
    /* P8.1: dev 自动 VSCode 启动测试--无需 GUI 交互即可验证 launch 链路 */
    static int g_autotest_state = 0;
    static u64 g_autotest_tsc = 0;

    while (!g_quit) {
        int need_redraw = 0;
        int moved = 0;

        /* P8.1: 自动启动测试（先 ping guest，不可用直接报告，不浪费 3s 等待） */
        if (g_tsc_per_sec && g_autotest_state == 0) {
            g_autotest_tsc = rdtsc();
            g_autotest_state = 1;
            /* P8.1 Fix: 先检测 guest 可用性 */
            int prc = lxc_guest_ping();
            if (prc != 0) {
                slog("[autotest] SKIP: Linux guest not running (no VMX/KVM)");
                slog("[autotest] To test VSCode: use WSL2+KVM or Linux host with KVM");
                g_autotest_state = 3;  /* 跳过测试 */
            } else {
                slog("[autotest] guest OK, waiting 3s before vscode_launch");
            }
        }
        if (g_autotest_state == 1 && g_tsc_per_sec) {
            u64 elapsed = rdtsc() - g_autotest_tsc;
            if (elapsed >= (u64)g_tsc_per_sec * 3) {
                slog("[autotest] triggering vscode_launch");
                g_autotest_state = 2;
                vscode_launch();
            }
        }
        if (g_autotest_state == 2 && g_tsc_per_sec) {
            u64 elapsed = rdtsc() - g_autotest_tsc;
            /* 15s 后报告结果 */
            if (elapsed >= (u64)g_tsc_per_sec * 15) {
                if (g_vscode_state == 2) {
                    slog("[autotest] SUCCESS: scanout online, VSCode running");
                } else if (g_vscode_state == 1) {
                    slog("[autotest] TIMEOUT: still launching");
                    slog_num("[autotest] phase=", g_launch_phase);
                } else {
                    slog("[autotest] FAIL: vscode_state=0");
                    slog_num("[autotest] phase=", g_launch_phase);
                }
                g_autotest_state = 3;
            }
        }

        /* 鼠标：排空所有数据包 */
        while (ps2_mouse_poll()) moved = 1;

        if (g_left_pressed) {
            g_left_pressed = 0;
            if (on_left_press()) need_redraw = 1;
        }
        if (g_right_pressed) {
            g_right_pressed = 0;
            if (on_right_press()) need_redraw = 1;
        }
        if (g_left_released) {
            g_left_released = 0;
            if (g_dragging || g_drag_slider) {
                g_dragging = 0;
                g_drag_win = -1;
                g_drag_slider = 0;
                need_redraw = 1;
            }
        }

        /* 窗口拖拽 */
        if (moved && g_dragging && g_drag_win >= 0) {
            desktop_window *w = win_find(g_drag_win);
            if (w) {
                w->x = g_mouse_x - g_drag_off_x;
                w->y = g_mouse_y - g_drag_off_y;
                if (w->y < KATE_TOPBAR_H) w->y = KATE_TOPBAR_H;
                if (w->x < 0 - w->w + 40) w->x = 0 - w->w + 40;
                if (w->x > (int)g_fb_w - 40) w->x = (int)g_fb_w - 40;
                if (w->y > (int)g_fb_h - KATE_TASKBAR_H - 20)
                    w->y = (int)g_fb_h - KATE_TASKBAR_H - 20;
            }
            need_redraw = 1;
        }

        /* 滑块拖拽 */
        if (moved && g_drag_slider == 1) { slider_set(g_vol_rect, &g_vol, g_mouse_x); need_redraw = 1; }
        if (moved && g_drag_slider == 2) { slider_set(g_bri_rect, &g_bright, g_mouse_x); need_redraw = 1; }

        /* 键盘：排空 */
        for (;;) {
            u8 st = inb(0x64);
            if (!(st & 1) || (st & 0x20)) break;
            u8 sc = inb(0x60);
            if (handle_key(sc)) need_redraw = 1;
        }

        /* RTC 秒 tick：DASHBOARD 状态栏时钟/UP 计时刷新 */
        {
            int sec = rtc_day_sec();
            if (sec != g_last_sec) {
                g_last_sec = sec;
                if (g_page == 1 && !g_taskview) need_redraw = 1;
            }
        }

        /* VSCode Phase 3/7: IDE attached 时轮询 guest scanout dirty，
         * 有新帧则触发重绘以 blit 到 framebuffer。查询结果缓存进 g_so
         * （查询清零 UTSM 侧 dirty + dirty rect），blit 据此局部拷贝。 */
        if (g_page == 2 && g_ide_attached && g_input_forward_enabled &&
            g_lxc_svc && g_lxc_svc->gpu_get_scanout_info) {
            if (g_lxc_svc->gpu_get_scanout_info(&g_so) == 0) {
                g_so_valid = 1;
                if (g_so.dirty) need_redraw = 1;
            }
        }

        /* P7.9: FPS 统计（每秒更新，基于 TSC） */
        if (g_tsc_per_sec && g_prof_fps_tsc) {
            u64 elapsed = rdtsc() - g_prof_fps_tsc;
            if (elapsed >= g_tsc_per_sec) {
                g_prof_fps = (int)(g_prof_frames * g_tsc_per_sec / elapsed);
                g_prof_frames = 0;
                g_prof_fps_tsc = rdtsc();
                if (g_page == 2) need_redraw = 1;  /* 刷新叠加显示 */
            }
        } else if (g_tsc_per_sec) {
            g_prof_fps_tsc = rdtsc();
        }
        if (need_redraw && g_page == 2) g_prof_frames++;

        if (need_redraw) {
            /* 脏矩形渲染：合并脏矩形，只 flip 变化区域 */
            if (g_full_redraw || g_dirty_count == 0) {
                /* 全屏重绘 */
                redraw_all();
                flip_buffer();
            } else {
                /* 合并脏矩形 */
                int dx, dy, dw, dh;
                merge_dirty_rects(&dx, &dy, &dw, &dh);
                
                /* 重绘并 flip 脏区域 */
                if (dw > 0 && dh > 0) {
                    /* 清除脏矩形标记 */
                    clear_dirty_rects();
                    
                    /* 重绘整个屏幕（简化：暂不支持局部重绘） */
                    redraw_all();
                    
                    /* Flip 合并后的脏区域 */
                    flip_rect(dx, dy, dw, dh);
                }
            }
        } else if (moved) {
            /* 仅光标移动：局部 flip（双缓冲 → 无闪烁） */
            int old_x = g_cursor_old_x, old_y = g_cursor_old_y;
            cursor_restore_bg();
            flip_rect(old_x, old_y, CURSOR_SIZE, CURSOR_SIZE);
            cursor_save_bg(g_mouse_x, g_mouse_y);
            cursor_draw(g_mouse_x, g_mouse_y);
            flip_rect(g_mouse_x, g_mouse_y, CURSOR_SIZE, CURSOR_SIZE);
        }

        __asm__("pause");
    }

    slog("exit");
    /* 返回到 DSK */
}
