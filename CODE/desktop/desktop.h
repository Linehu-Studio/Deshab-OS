/* desktop.h — Deshab 桌面跨模块共享契约（M0 模块化拆分）
 *
 * 模块划分（M0：纯平移，零功能变更）：
 *   main.c          组合根：dsk_entry、服务探测、FUCK、boot 屏、主事件循环
 *   input.c         PS/2 鼠标键盘、光标、RTC/TSC 时钟
 *   wm.c            窗口管理（win_*）+ 窗口装饰
 *   compositor.c    脏矩形、双缓冲 flip、全屏重绘
 *   apps_builtin.c  内置应用 bash/editor/calc + FILES 面板
 *   appreg.c        应用注册表（内置/LINUXAPP/PEAPPS/user desktop）+ 图标绘制
 *   launcher.c      统一启动器 + 外部 ELF 加载 + PE 窗口运行时
 *   lxc.c           Linux guest / IDE / VSCode 启动链路 + 性能 profiling
 *   shell.c         工作区/页面状态机 + 任务视图/右键菜单 + 事件路由
 *   pages.c         KATE 风格页面绘制（topbar/dashboard/ide/custom/desktop）
 *   f32io.c         fat32_io.h 单编译单元封装（4MB 缓冲唯一持有者）
 *   ascii_font.c    ASCII 位图字体单副本
 *   zhfont.c        中文位图字库（.dbf）加载与渲染（M1）
 *   wallpaper.c     壁纸背景层缓存（M1）
 *
 * 依赖单向：main → shell → (pages/appreg/launcher/lxc) → wm/compositor → input。
 * 全局状态按"最 owning 模块定义、此处 extern"原则组织。
 */
#ifndef DESHAB_DESKTOP_H
#define DESHAB_DESKTOP_H

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/linux_compat.h"   /* VSCode Phase 2: input_forward_* */
#include "../UTSM/include/utsm/pe.h"             /* P5d: PE 窗口模式 run_windowed/inject_* */
/* ascii_font.h 须在 deshab_ui.h 之前（g_ascii）；du_context 在 app_ctx 里用到 */
#include "ascii_font.h"
#include "../UTSM/include/utsm/deshab_ui.h"

/* block 设备函数类型（从 kernel_api + 0xA8 获取 block_api，read @ +0x10, write @ +0x18） */
typedef int (*desktop_block_read_fn)(u32 index, u64 lba, u32 count, void *buffer);
typedef int (*desktop_block_write_fn)(u32 index, u64 lba, u32 count, const void *buffer);

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

#define COM1 0x3F8

/* ---- 小工具（inline，各模块共享） ---- */

static __inline__ void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static __inline__ u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static __inline__ void sputc(char c) {
    for (unsigned int i = 0; i < 100000; i++) { if (inb(COM1+5)&0x20) break; }
    outb(COM1, (unsigned char)c);
}
static __inline__ void swrite(const char *s) { while(*s) { if(*s=='\n')sputc('\r'); sputc(*s++); } }
static __inline__ void slog(const char *s) { swrite("[desktop] "); swrite(s); swrite("\n"); }

static __inline__ int kstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static __inline__ void kstrcpy(char *d, const char *s, int cap) {
    int i = 0;
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static __inline__ void kstrcat(char *d, const char *s, int cap) {
    int n = kstrlen(d);
    if (n < cap) kstrcpy(d + n, s, cap - n);
}

/* P7.9: u32 -> decimal string（返回长度） */
static __inline__ int u32dec(u32 v, char *out) {
    char tmp[10];
    int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0) { tmp[n++] = '0' + (char)(v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static __inline__ u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* ---- 常量（Winux-Kate 布局） ---- */

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
#define MAX_ICONS          24
#define MAX_APPS           24

/* 应用固定状态地址（单实例策略 → 每类应用唯一实例） */
#define ADDR_TERM1   0x4000000ULL
#define ADDR_TERM2   0x4100000ULL
#define ADDR_TERMWIN 0x4200000ULL
#define ADDR_EDDASH  0x5000000ULL
#define ADDR_EDWIN   0x5100000ULL
#define ADDR_CALC    0x6000000ULL

/* 双缓冲：渲染到 sprite buffer，完成后一次性 flip 到真实 framebuffer */
#define SPRITE_BUF_ADDR 0x7000000ULL          /* 112MB，在 identity-mapped 区域 */

#define PE_SURFACE_ADDR  0x7800000ULL  /* 120MB：sprite buf(112MB, ≤8.3MB) 之上 */
#define PE_SURFACE_MAX   (8u * 1024u * 1024u)
#define PE_WM_CLOSE      0x0010u       /* shim Win32 WM_CLOSE */

/* ---- 事件系统 ---- */

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

/* ---- 窗口 / 应用 ---- */

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
    /* M2: 1 = 来自 user/desktop/*.lnk 的用户快捷方式（桌面只显示这些） */
    int user_desktop;
} app_descriptor;

/* 桌面图标 */
typedef struct {
    int    app_id;
    int    x, y;
    const char *label;
    int    selected;
} desktop_icon;

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

/* ---- 内置应用状态 ---- */

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

typedef struct {
    char display[32];
    int  display_len;
    i64  accumulator;
    i64  current;
    int  op;
    int  new_number;
} calc_state;

/* ---- 脏矩形 ---- */

#define MAX_DIRTY_RECTS  16

typedef struct {
    int x, y, w, h;
    int active;
} dirty_rect;

/* ---- FAT32 单 TU 封装（f32io.c 持有 fat32_io.h 的 4MB 缓冲） ---- */
#include "f32io.h"
/* 纯字符串助手（无状态，可安全多 TU 包含） */
#include "../tools/fat32_lfn.h"

/* ============================================================
 *  全局状态 extern（定义见各 owning 模块）
 * ============================================================ */

/* ---- main.c：渲染上下文 / 服务指针 ---- */
extern du_context g_fb;
extern u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch;
extern const void *g_kernel_api;
extern u8 *g_real_fb;
extern desktop_block_read_fn  g_block_read;
extern desktop_block_write_fn g_block_write;
extern const dsk_boot_context *g_boot_ctx;
extern const linux_compat_service *g_lxc_svc;
extern int g_input_forward_enabled;
extern const pe_service *g_pe_svc;

/* ---- input.c：输入 / 时钟 ---- */
extern int g_mouse_x, g_mouse_y;
extern int g_mouse_btn;      /* bit0=左 bit1=右 */
extern int g_left_pressed, g_left_released, g_right_pressed;
extern int g_cursor_saved;
extern int g_cursor_old_x, g_cursor_old_y;
extern int g_shift, g_ctrl, g_alt, g_e0;
extern int g_rtc_boot_sec;   /* 开机时刻（秒级当天秒） */
extern int g_last_sec;
extern u64 g_tsc_per_sec;

/* ---- wm.c：窗口 ---- */
extern desktop_window g_windows[MAX_WINDOWS];
extern int g_win_count;
extern int g_focused_win;
extern int g_dragging;
extern int g_drag_win;
extern int g_drag_off_x, g_drag_off_y;

/* ---- compositor.c：脏矩形 ---- */
extern int g_dirty_count;
extern int g_full_redraw;

/* ---- apps_builtin.c：FILES 面板 ---- */
extern fentry g_files[MAX_FILES];
extern int g_file_count;
extern int g_file_sel;
extern int g_files_loaded;

/* ---- appreg.c：应用表 / 图标 ---- */
extern app_descriptor g_apps[MAX_APPS];
extern int g_app_count;
extern desktop_icon g_icons[MAX_ICONS];
extern int g_icon_count;
extern int g_vscode_app_id;   /* 第一个名为 "vscode" 的注册项 */

/* ---- launcher.c：PE 运行时 ---- */
extern int   g_pe_running;
extern int   g_pe_win_id;
extern u8   *g_pe_surf;
extern int   g_pe_surf_w, g_pe_surf_h;

/* ---- lxc.c：IDE / guest / profiling ---- */
extern int g_ide_attached;
extern int g_vscode_state;       /* 0=未运行 1=启动中 2=运行中 */
extern int g_launch_phase;
extern char g_launch_status[64];
extern u64  g_launch_tsc;
extern int  g_launch_exec_rc;
extern int  g_launch_pgrep_rc;
extern char g_ide_tab_label[16];
extern u64 g_prof_frames;
extern u64 g_prof_fps_tsc;
extern int g_prof_fps;
extern u64 g_prof_blit_total;
extern u64 g_prof_blit_count;
extern u64 g_prof_blit_skip;
extern u64 g_prof_blit_full;
extern int g_prof_last_blit_us;
extern struct linux_compat_scanout_info g_so;
extern int g_so_valid;
extern int g_so_full_repaint;

/* ---- main.c：M2 Fluent 开关（FUCK [desktop] fluent=，缺省 1） ---- */
extern int g_fluent;

/* ---- shell.c：外壳状态 / 命中矩形 ---- */
extern int g_page;                 /* 1..3 内置, 4..8 自定义 */
extern int g_taskview;
extern int g_dash_focus;           /* 0=t1 1=t2 2=ed 3=files */
extern workspace g_ws[MAX_WS];
extern int g_ws_count;
extern int g_ws_cur;
extern cpage g_cpages[MAX_CPAGES];
extern int g_cpage_count;
extern int g_ctx_open;
extern int g_ctx_x, g_ctx_y;
extern int g_ctx_win;
extern int g_vol;
extern int g_bright;
extern int g_bright_applied;
extern int g_drag_slider;          /* 1=vol 2=bright */
extern int g_vol_rect[4];
extern int g_bri_rect[4];
extern int g_quit;                 /* EXIT 按钮 → 返回 DSK */
extern u64 g_last_click_tsc;
extern int g_last_click_x, g_last_click_y;
extern int g_dash_rect[4][4];          /* 4 面板 x,y,w,h */
extern int g_save_rect[4];
extern int g_pg_rect[8][4];            /* 页面切换按钮 */
extern int g_pg_rm_rect[8][4];
extern int g_pgadd_rect[4];
extern int g_tvbtn_rect[4];            /* 任务视图按钮 */
extern int g_ws_rect[MAX_WS][4];
extern int g_wsadd_rect[4];
extern int g_tb_rect[MAX_WINDOWS][4];  /* 任务栏项 */
extern int g_tb_close_rect[MAX_WINDOWS][4];
extern int g_tb_quit_rect[4];
extern int g_tb_adopt_rect[4];           /* 收纳窗口按钮 */
extern int g_tv_card[MAX_WS + 1][4];   /* 任务视图卡片(+add) */
extern int g_tv_close[MAX_WS][4];
extern int g_ctx_rect[4];
extern int g_ctx_item[MAX_WS + 1][4];
extern int g_ctx_item_ws[MAX_WS + 1];   /* 菜单行 → 真实工作区索引，-1=新建 */
extern int g_ide_host[4];
extern int g_ide_tabx[4];              /* IDE 标签 × */
extern int g_ide_new_rect[4];          /* IDE "+ NEW IDE" 按钮 */
extern int g_cp_host[4];               /* 自定义页宿主 */
extern int g_cp_new_rect[4];           /* 自定义页 "+ NEW INSTANCE" 按钮 */

/* ============================================================
 *  跨模块函数原型
 * ============================================================ */

/* ---- input.c ---- */
void rtc_read(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss);
int  rtc_day_sec(void);
void fmt_hms(char *out, int sec);
void tsc_calibrate(void);
void ps2_mouse_init(void);
int  ps2_mouse_poll(void);
char scan_to_ascii(u8 sc, int shift);
u16  scancode_to_linux_keycode(u8 base, int e0);
void cursor_save_bg(int mx, int my);
void cursor_restore_bg(void);
void cursor_draw(int mx, int my);

/* ---- wm.c ---- */
desktop_window *win_find(int id);
desktop_window *win_find_at(int mx, int my);
void win_focus(desktop_window *w);
desktop_window *win_create(int app_id, const char *title, int x, int y, int w, int h);
void win_destroy(desktop_window *w);
int  win_titlebar_h(void);        /* M2：Fluent 36 / KATE 28 */
int  win_hit_titlebar(desktop_window *w, int mx, int my);
void win_close_request(desktop_window *w);
int  win_hit_close_btn(desktop_window *w, int mx, int my);
int  win_hit_min_btn(desktop_window *w, int mx, int my);   /* M2 Fluent */
int  win_hit_max_btn(desktop_window *w, int mx, int my);   /* M2 Fluent */
void wm_set_btn_hover(int btn);   /* 0=无 1=min 2=max 3=close */
int  wm_detect_btn_hover(desktop_window *w, int mx, int my);
int  ws_win_count(int ws);
void draw_window_frame(desktop_window *w);

/* ---- compositor.c ---- */
void mark_dirty(int x, int y, int w, int h);
void merge_dirty_rects(int *out_x, int *out_y, int *out_w, int *out_h);
void clear_dirty_rects(void);
void flip_buffer(void);
void flip_rect(int x, int y, int w, int h);
void anim_delay_poll_mouse(u32 ms);
void redraw_all(void);

/* ---- apps_builtin.c ---- */
extern const char *BASH_PROMPT;
void bash_reset(bash_state *s, int w, int h);
void bash_puts(bash_state *s, const char *str, u32 color);
void bash_key(bash_state *s, u8 sc, int shift);
void bash_draw_to(bash_state *s, int cx, int cy, int cw, int ch);
void editor_reset(editor_state *s, const char *sample);
void editor_key(editor_state *s, u8 sc, int shift);
void editor_draw_to(editor_state *s, int cx, int cy, int cw, int ch);
void editor_save(void);
void files_refresh(void);
void files_open(int idx);
void *editor_on_create(app_ctx *ctx);
void  editor_on_event(void *state, app_ctx *ctx, desktop_event *ev);
void  editor_on_draw(void *state, app_ctx *ctx);
void  editor_on_destroy(void *state);
void *calc_on_create(app_ctx *ctx);
void  calc_on_event(void *state, app_ctx *ctx, desktop_event *ev);
void  calc_on_draw(void *state, app_ctx *ctx);
void  calc_on_destroy(void *state);

/* ---- appreg.c ---- */
void register_apps(void);
void setup_desktop_icons(void);
void draw_app_icon(du_context *ctx, int app_id, int cx, int cy);
void draw_circle_outline(du_context *ctx, int cx, int cy, int r, u32 color);

/* ---- launcher.c ---- */
void launch_external_elf(const char *name11);
void fill_app_ctx(app_ctx *ctx, desktop_window *win);
void launch_app(int app_id);
void launch_pe_app(int app_id);
void pe_win_on_draw(void *state, app_ctx *ctx);

/* ---- lxc.c ---- */
void slog_num(const char *prefix, int rc);
int  lxc_guest_ping(void);
void guest_gui_launch(const char *path, int argc, const char *const *argv);
void launch_linux_app(int app_id);
void vscode_launch(void);
void ide_attach_invalidate(void);
const struct linux_compat_scanout_info *ide_scanout_state(void);
void blit_scanout_to_ide(int hx, int hy, int hw, int hh);
void desktop_launch_pending_native(void);

/* ---- shell.c ---- */
void page_switch(int p);
int  ctx_list_build(int *map);
int  on_left_press(void);
int  on_right_press(void);
int  handle_key(u8 sc);
void slider_set(const int rect[4], int *val, int mx);

/* ---- pages.c ---- */
void draw_topbar(void);
void draw_dashboard(void);
void draw_ide(void);
void draw_desktop_page(void);
void draw_custom_page(int cp_idx);
void draw_taskview(void);
void draw_ctx_menu(void);

/* ---- taskbar.c（M2 Fluent 悬浮任务栏） ---- */
void taskbar_draw(void);
int  taskbar_click(int mx, int my);
int  taskbar_init(int (*read_to)(const char *, unsigned char *,
                                 unsigned int, unsigned int *));   /* M2: logo 纹理 */
extern int g_tb2_start_rect[4];
extern int g_tb2_tv_rect[4];
extern int g_tb2_app_rect[MAX_WINDOWS][4];
extern int g_tb2_tray_rect[4];
extern int g_tb2_net_rect[4];
extern int g_tb2_vol_rect[4];
extern int g_tb2_pwr_rect[4];
extern int g_tb2_clk_rect[4];

/* ---- main.c ---- */
void dsk_entry(const dsk_boot_context *ctx);

/* ---- 内联小绘制（依赖 g_fb extern，放头文件末尾） ---- */

/* 矩形命中 */
static __inline__ int rect_hit(const int r[4], int mx, int my) {
    return r[2] > 0 && mx >= r[0] && mx < r[0] + r[2] &&
           my >= r[1] && my < r[1] + r[3];
}

/* 矩形内居中文字 */
static __inline__ void draw_centered(const char *s, int cx, int y, u32 fg, u32 bg) {
    int l = kstrlen(s);
    du_draw_string(&g_fb, s, cx - l * (int)DU_ASCII_STEP / 2, y, fg, bg, DU_ASCII_STEP);
}

/* 虚线矩形（ide-host / custom host） */
static __inline__ void draw_dashed_rect(int x, int y, int w, int h, u32 color) {
    for (int i = x; i < x + w; i += 8) {
        du_fill_rect(&g_fb, i, y, 4, 1, color);
        du_fill_rect(&g_fb, i, y + h - 1, 4, 1, color);
    }
    for (int i = y; i < y + h; i += 8) {
        du_fill_rect(&g_fb, x, i, 1, 4, color);
        du_fill_rect(&g_fb, x + w - 1, i, 1, 4, color);
    }
}

#endif /* DESHAB_DESKTOP_H */
