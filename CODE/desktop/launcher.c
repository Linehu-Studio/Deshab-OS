/* launcher.c — 统一启动器 + 外部 ELF 加载 + PE 窗口运行时（M0 自 main.c 平移）
 */
#include "desktop.h"

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

void launch_external_elf(const char *name11) {
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file_w(name11, &data, &size) != 0) {
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

void fill_app_ctx(app_ctx *ctx, desktop_window *win) {
    int tb = win_titlebar_h();
    ctx->fb = &g_fb;
    ctx->win = win;
    ctx->client_x = win->x + BORDER_W;
    ctx->client_y = win->y + tb;
    ctx->client_w = win->w - 2 * BORDER_W;
    ctx->client_h = win->h - tb - BORDER_W;
    ctx->should_exit = 0;
    ctx->block_read = g_block_read;
    ctx->block_write = g_block_write;
    ctx->kernel_api = g_kernel_api;
}

void launch_app(int app_id) {
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

/* PE 运行时状态 */
int   g_pe_running = 0;
int   g_pe_win_id = -1;
u8   *g_pe_surf = 0;
int   g_pe_surf_w = 0, g_pe_surf_h = 0;
static int   g_pe_state_dummy = 0;     /* draw_desktop_page 要求 app_state 非空 */

/* PE 窗口 on_draw：把 shim 渲染的 surface blit 到客户区（32bpp 行拷贝） */
void pe_win_on_draw(void *state, app_ctx *ctx) {
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
    int pe_tb = win_titlebar_h();
    int cx0 = pw->x + BORDER_W, cy0 = pw->y + pe_tb;
    int ccw = pw->w - 2 * BORDER_W, cch = pw->h - pe_tb - BORDER_W;
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
void launch_pe_app(int app_id) {
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
    if (f32_read_path_lfn_w(app->pe_file, &data, &size) != 0) {
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
        for (u64 i = 0; i < n; i++) p[i] = 0xFFF3F4F6u;
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
