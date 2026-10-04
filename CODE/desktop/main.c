/* main.c — Deshab Desktop Manager 组合根（M0 模块化拆分后）
 *
 * 原 4610 行单文件已拆分为 13 个模块（见 desktop.h 头注释）。
 * 本文件保留：dsk_entry 入口、服务指针探测（lxc/pe/block）、boot 屏、
 * 主事件循环。功能与拆分前完全一致（零功能变更）。
 *
 * 原 "Winux-Kate 结构完整复刻" 布局说明见 pages.c 头注释；
 * M2-M4 将按 Win11 Fluent 桌面重构计划逐页替换。
 */
#include "ascii_font.h"  /* 顺序敏感：先于 desktop.h（g_ascii 导出） */
#include "desktop.h"
#include "zhfont.h"       /* M1: 中文位图字库（dbf 加载 + 中英混排渲染） */
#include "wallpaper.h"   /* M2: 壁纸背景层缓存 */
#include "textures.h"    /* M2: 启动背景/开机 logo/开始菜单 logo 纹理 */

/* ---- 全局状态（渲染上下文 / 服务指针，各模块 extern） ---- */
du_context g_fb;
u64 g_fb_addr, g_fb_w, g_fb_h, g_fb_pitch;
const void *g_kernel_api;
u8 *g_real_fb = 0;                     /* 真实 framebuffer 地址 */
desktop_block_read_fn  g_block_read;
desktop_block_write_fn g_block_write;
const dsk_boot_context *g_boot_ctx = 0;

/* M2: Win11 Fluent 桌面开关（FUCK [desktop] fluent=，缺省 1）。
 * 1 = 壁纸背景 + Fluent 圆角窗口 + 悬浮任务栏；0 = 旧 KATE 桌面。 */
int g_fluent = 1;

/* VSCode Phase 2: Linux guest 输入转发。
 * g_lxc_svc 由 dsk_entry() 从 boot context reserved[5] 读取；
 * g_input_forward_enabled 在 IDE 标签 attach/detach 时联动置位，
 * 开启后 PS/2 鼠标键盘事件经 virtio-input 注入 Linux guest（VSCode 运行其中）。 */
const linux_compat_service *g_lxc_svc = 0;
int g_input_forward_enabled = 0;

/* P5d: PE 兼容层服务指针（dsk_loader 在 reserved[4] 写入，magic 校验）。 */
const pe_service *g_pe_svc = 0;

static int g_booted = 0;

/* ============================================================
 *  M2: FUCK 配置解析（[desktop] fluent=）
 *  复用 dsk 的 FAT32 路径读取（f32_read_path_lfn_w），INI 最小解析。
 * ============================================================ */

static void load_fuck_config(void) {
    u8 *data = 0;
    u32 size = 0;
    if (f32_read_path_lfn_w("system/deshab64/FUCK", &data, &size) != 0) {
        slog("[cfg] FUCK not found, defaults");
        return;
    }
    /* 逐行找 [desktop] 段的 fluent= 键 */
    int in_dsk = 0;
    u32 pos = 0;
    while (pos < size) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        u32 len = eol - pos;
        const char *line = (const char *)data + pos;
        if (len > 0 && line[0] != '#' && line[0] != ';') {
            if (len > 1 && line[0] == '[') {
                in_dsk = (len >= 9 && line[1] == 'd' && line[2] == 's' &&
                          line[3] == 'k' && line[4] == ']');
            } else if (in_dsk && len >= 7 &&
                       line[0]=='f' && line[1]=='l' && line[2]=='u' &&
                       line[3]=='e' && line[4]=='n' && line[5]=='t' && line[6]=='=') {
                int v = (len > 7 && line[7] == '0') ? 0 : 1;
                if (g_fluent != v) {
                    g_fluent = v;
                    slog(v ? "[cfg] fluent=1 (Win11 desktop)" :
                             "[cfg] fluent=0 (KATE fallback)");
                }
            }
        }
        pos = eol;
        while (pos < size && (data[pos] == '\n' || data[pos] == '\r')) pos++;
    }
}

/* ============================================================
 *  登录 → 桌面 crossfade（渐隐渐显）
 *  登录最后一帧已在真实 fb 上；桌面首帧已由 redraw_all() 绘入
 *  sprite buffer。复用 WALLPAPER_STAGING（wallpaper_init 后已空闲）
 *  保存登录帧，按 alpha 混合 16 步直写真实 fb。
 * ============================================================ */

#define XFADE_ADDR   WALLPAPER_STAGING_ADDR   /* 8MB 区，init 后空闲 */
#define XFADE_STEPS  16

static void busy_delay(u32 cycles) {
    for (volatile u32 i = 0; i < cycles; i++) __asm__("pause");
}

static void crossfade_login_to_desktop(void) {
    int W = (int)g_fb_w, H = (int)g_fb_h;
    u32 *src = (u32 *)XFADE_ADDR;          /* 登录帧 */
    const u32 *dst = (const u32 *)g_fb.fb; /* 桌面帧（sprite buffer） */
    u32 *out = (u32 *)g_real_fb;
    u64 n = (u64)W * (u64)H;

    /* 1. 保存登录最后一帧 */
    {
        u64 pitch_u32 = g_fb_pitch / 4;
        for (u64 y = 0; y < g_fb_h; y++) {
            const u32 *s = (const u32 *)((const u8 *)g_real_fb + y * g_fb_pitch);
            u32 *d = (u32 *)((u8 *)src + y * (u64)W * 4);
            for (u64 x = 0; x < g_fb_w; x++) d[x] = s[x];
        }
        (void)pitch_u32;
    }

    /* 2. 桌面首帧绘入 sprite buffer（由调用方 redraw_all() 完成后进入本函数） */

    /* 3. alpha 混合直写真实 fb：out = src*(1-a) + dst*a */
    for (int step = 1; step <= XFADE_STEPS; step++) {
        u32 a = (u32)(step * 256 / XFADE_STEPS);
        u32 ia = 256 - a;
        for (u64 i = 0; i < n; i++) {
            u32 s = src[i], d = dst[i];
            u32 sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            out[i] = 0xFF000000u |
                     (((sr * ia + dr * a) >> 8) << 16) |
                     (((sg * ia + dg * a) >> 8) << 8) |
                     ((sb * ia + db * a) >> 8);
        }
        busy_delay(40000);
    }
}

/* ============================================================
 *  严格错误策略插桩（D 系列）：错误不再降级，整屏 fatal + 错误码。
 *  每个插桩点唯一 symbol（DESK-E<nn>）。
 * ============================================================ */

static void desktop_fatal(const char *sym) {
    /* 严格错误策略：串口留档 + 深红 fatal 屏（ASCII 18px 直写真实 fb）+ 停机 */
    slog(sym);
    if (g_real_fb && g_fb_w && g_fb_h) {
        u32 *fb = (u32 *)g_real_fb;
        u64 pitch_u32 = g_fb_pitch / 4;
        for (u64 y = 0; y < g_fb_h; y++)
            for (u64 x = 0; x < g_fb_w; x++)
                fb[y * pitch_u32 + x] = 0xFF3A0A0Au;   /* 深红 */
        /* 居中绘制 symbol（ASCII 字符走 11x18 位图，其余跳过） */
        int len = 0;
        while (sym[len]) len++;
        int cw = 11, ch = 18;
        int x0 = (int)(g_fb_w - (u64)len * cw) / 2;
        int y0 = (int)g_fb_h / 2 - ch / 2;
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)sym[i];
            if (c < 32 || c > 126) continue;
            const unsigned char *g = g_ascii[c - 32];
            for (int py = 0; py < ch; py++) {
                u32 *row = fb + (u64)(y0 + py) * pitch_u32;
                for (int px = 0; px < cw; px++) {
                    if (g[py * cw + px])
                        row[x0 + i * cw + px] = 0xFFF2F2F2u;   /* 白 */
                }
            }
        }
    }
    for (;;) __asm__("cli; hlt");
}

/* ============================================================
 *  主入口
 * ============================================================ */

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    __asm__ volatile("cli");
    slog("boot");

    if (!ctx || ctx->magic != 0x44534B31424F4F54ULL) {
        desktop_fatal("DSK-E01-BADCONTEXT");
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
    f32_io_init(g_block_read, g_block_write);

    /* M1: 中文位图字库加载（system/font/simhei_16.dbf → 0x8000000）。
     * 失败阻塞启动。 */
    {
        int zrc = zhfont_init(f32_read_path_lfn_to_w);
        if (zrc == 0) {
            slog("[zhfont] dbf loaded (16px ok)");
        } else {
            slog_num("[zhfont] dbf load failed rc=", zrc);
            desktop_fatal("DESK-E04 FONT PACK MISSING");
        }
    }

    /* M2: FUCK 配置（[desktop] fluent=）→ 壁纸初始化（Fluent 路径需要） */
    load_fuck_config();
    if (g_fluent) {
        int wrc = wallpaper_init(f32_read_path_lfn_to_w, (int)g_fb_w, (int)g_fb_h);
        if (wrc == 0) {
            slog("[wallpaper] loaded + scaled");
        } else {
            slog_num("[wallpaper] load failed rc=", wrc);
            /* 严格错误策略：材质包（壁纸）缺失不降级，fatal 屏 + 错误码 */
            desktop_fatal("DESK-E02 TEXTURE PACK MISSING");
        }
        /* M2: 开始菜单 logo（textures/startMenuLogo.rgba → 0x8700000） */
        if (taskbar_init(f32_read_path_lfn_to_w) == 0)
            slog("[taskbar] startMenu logo loaded");
        else {
            /* 严格错误策略：材质包（开始菜单 logo）缺失不降级 */
            desktop_fatal("DESK-E03 STARTMENU LOGO MISSING");
        }
        g_page = 3;   /* Fluent 默认进 DESKTOP 页 */
    }

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

    /* 桌面首帧绘入 sprite buffer，然后从登录帧渐隐渐显到桌面（无 boot 屏） */
    redraw_all();
    crossfade_login_to_desktop();
    g_booted = 1;

    slog("desktop ready");
    desktop_launch_pending_native();

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
                slog("[autotest] guest OK, waiting 90s before vscode_launch (KDE serial 2 first)");
            }
        }
        if (g_autotest_state == 1 && g_tsc_per_sec) {
            u64 elapsed = rdtsc() - g_autotest_tsc;
            if (elapsed >= (u64)g_tsc_per_sec * 90) {
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

        if (inb(COM1 + 5) & 1) {
            unsigned char ch = inb(COM1);
            if (ch == '2') {
                int ki;
                slog("serial 2: launch KDE");
                for (ki = 0; ki < g_app_count; ki++) {
                    if (g_apps[ki].name &&
                        fat32_lfn_streq_ci(g_apps[ki].name, "kde")) {
                        launch_app(ki);
                        need_redraw = 1;
                        break;
                    }
                }
            }
        }

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
