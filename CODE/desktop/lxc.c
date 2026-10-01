/* lxc.c — Linux guest / IDE / VSCode 启动链路 + 性能 profiling（M0 自 main.c 平移）
 */
#include "desktop.h"

/* ---- 全局状态 ---- */
int g_ide_attached = 0;

/* VSCode Phase 6: guest GUI 状态机。
 * 0 = 未运行（IDE host 显示启动提示）
 * 1 = 启动中（X server/VSCode spawn 已发出，等 scanout 上线）
 * 2 = 运行中（scanout enabled，blit guest 帧） */
int g_vscode_state = 0;

/* P8.1: VSCode 启动插桩--分阶段状态跟踪 + 串口日志 + 屏幕显示。
 * g_launch_phase: 0=idle 1=X启动 2=X socket等待 3=app spawn 4=pgrep验证 5=等scanout 6=running 7=失败
 * g_launch_status: 屏幕显示的状态字符串（IDE host 区域） */
int g_launch_phase = 0;
char g_launch_status[64] = "";
u64  g_launch_tsc = 0;        /* 启动开始 TSC（测各阶段耗时） */
int  g_launch_exec_rc = 0;    /* exec_async 返回码 */
int  g_launch_pgrep_rc = -1;  /* pgrep 验证结果（0=找到, 1=未找到）*/

/* P7.4 多实例：IDE tab 动态标签——显示最近启动实例的 DISPLAY 名
 * （单 X :0 共享 scanout，多 VSCode 进程同屏；标签指示最后 attach 者）。 */
char g_ide_tab_label[16] = "VSCODE";

/* P7.9: 性能 profiling（TSC 计时，每秒更新 FPS 显示）。
 * 在 IDE 页面叠加显示 FPS / blit 耗时 / dirty 跳过率。 */
u64 g_prof_frames = 0;        /* 自上次 FPS 更新以来的帧数 */
u64 g_prof_fps_tsc = 0;       /* 上次 FPS 计算的 TSC */
int g_prof_fps = 0;           /* 当前 FPS（每秒更新） */
u64 g_prof_blit_total = 0;    /* 累计 blit TSC */
u64 g_prof_blit_count = 0;    /* blit 调用次数 */
u64 g_prof_blit_skip = 0;     /* dirty=0 跳过的次数 */
u64 g_prof_blit_full = 0;     /* full repaint 次数 */
int g_prof_last_blit_us = 0;  /* 上次 blit 耗时（微秒） */

/* VSCode Phase 3/7: 把 virtio-gpu scanout 影子缓冲 blit 到 IDE host 区域。
 * scanout 为 1024×768 XRGB8888（stride=4096），与 framebuffer 同为 32bpp。
 * Phase 7 dirty rect 优化：主循环每帧把 scanout 状态缓存进 g_so（查询会
 * 清零 UTSM 侧 dirty），blit 只拷贝累积 dirty rect（scanout 坐标系并集），
 * sprite 双缓冲持久保留未变区域；attach/首帧/状态切换经 g_so_full_repaint
 * 全量重绘一次。 */
struct linux_compat_scanout_info g_so;
int g_so_valid = 0;          /* 1 = g_so 为本帧主循环 poll 的缓存 */
int g_so_full_repaint = 1;   /* 1 = 下次 blit 全量拷贝（初始/attach/上线） */

/* P8.1: slog + 数字（用于 rc/ec 日志，负数正确显示前导 -） */
void slog_num(const char *prefix, int rc) {
    char buf[48];
    kstrcpy(buf, prefix, 48);
    int n = kstrlen(buf);
    if (rc < 0) { buf[n++] = '-'; rc = -rc; }
    u32dec((u32)rc, buf + n);
    slog(buf);
}

/* VSCode Phase 3: IDE attach 时在 Linux guest 中启动 X server（Xfbdev）。
 * Xfbdev 通过 virtio-gpu 2D 设备合成帧到 surface pool scanout 影子缓冲，
 * desktop 每帧 blit 到 IDE host 区域。exec 用 sh -c "... &" 后台启动，
 * shell 立即退出被 daemon 回收，Xfbdev 由 init 收养继续运行。
 * Phase 6: pgrep 守卫保证幂等（多实例/重复 attach 不重起 X）。 */
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
int lxc_guest_ping(void) {
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
void guest_gui_launch(const char *path, int argc, const char *const *argv) {
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

void desktop_launch_pending_native(void) {
    char out[32];
    u64 ec = 0;
    int i, n;
    int app_id = -1;
    const char *argv[] = {
        "sh", "-c",
        "cat /tmp/deshab-native-app 2>/dev/null; rm -f /tmp/deshab-native-app /tmp/deshab-native"
    };
    if (!g_lxc_svc || !g_lxc_svc->exec) return;
    for (i = 0; i < 32; i++) out[i] = 0;
    if (g_lxc_svc->exec("/bin/sh", 3, argv, out, 31, 0, &ec) != 0) return;
    n = 0;
    while (n < 31 && out[n] && out[n] != '\n' && out[n] != '\r' && out[n] != ' ') n++;
    out[n] = 0;
    if (!out[0]) return;
    if (fat32_lfn_streq_ci(out, "kde") || fat32_lfn_streq_ci(out, "native")) return;
    for (i = 0; i < g_app_count; i++) {
        if (g_apps[i].name && fat32_lfn_streq_ci(g_apps[i].name, out)) {
            app_id = i;
            break;
        }
    }
    if (app_id < 0) return;
    slog("native launch from KDE:");
    slog(out);
    g_page = 3;
    launch_app(app_id);
}

/* VSCode Phase 5/6: 桌面图标启动 Linux 应用（LINUXAPP.CNF 注册项）。
 * 解析 args（空格分隔）后经共享路径 exec_async，并切到 IDE 页 attach。 */
void launch_linux_app(int app_id) {
    if (app_id < 0 || app_id >= g_app_count) return;
    app_descriptor *app = &g_apps[app_id];
    if (!app->linux_path) return;
    if (!g_lxc_svc || !g_lxc_svc->exec_async) {
        slog("linuxapp: lxc service unavailable");
        return;
    }

    if (fat32_lfn_streq_ci(app->name, "kde")) {
        const char *kargv[2];
        struct linux_compat_scanout_info si;
        u32 *fb = (u32 *)g_fb.fb;
        slog("kde: launching fullscreen Plasma");
        if (g_lxc_svc->exec) {
            const char *rmargv[] = { "sh", "-c", "rm -f /tmp/deshab-native /tmp/deshab-native-app" };
            u64 ec = 0;
            (void)g_lxc_svc->exec("/bin/sh", 3, rmargv, 0, 0, 0, &ec);
        }
        kargv[0] = app->linux_path;
        kargv[1] = 0;
        if (g_lxc_svc->exec_async(app->linux_path, 1, kargv) != 0) {
            slog("kde: exec_async failed");
            return;
        }
        {
            int scanout_logged = 0;
        for (;;) {
            u8 st = inb(0x64);
            if (st & 1) {
                if (!(st & 0x20)) {
                    u8 sc = inb(0x60);
                    if (sc == 0x01) {
                        slog("kde: aborted by Esc");
                        break;
                    }
                } else {
                    (void)inb(0x60);
                }
            }
            if (g_lxc_svc->run_slice) {
                int pump;
                for (pump = 0; pump < 4; pump++)
                    (void)g_lxc_svc->run_slice();
            }
            /* Do not exec test/sleep here: sync exec starves Plasma on the
             * only vCPU (native switch is Esc on the host loop). */
            if (g_lxc_svc->gpu_get_scanout_info &&
                g_lxc_svc->gpu_get_scanout_info(&si) == 0 &&
                si.enabled && si.host_vaddr && fb) {
                u32 row, copy_w;
                const u8 *src = (const u8 *)si.host_vaddr;
                u32 sw = si.width, sh = si.height, stride = si.stride;
                if (sw && sh && stride >= sw * 4) {
                    if (!scanout_logged) {
                        slog("KDE scanout w=");
                        slog_num("KDE scanout w=", (int)sw);
                        scanout_logged = 1;
                    }
                    for (row = 0; row < sh && row < (u32)g_fb.height; row++) {
                        copy_w = sw;
                        if (copy_w > (u32)g_fb.width) copy_w = (u32)g_fb.width;
                        {
                            u8 *dd = (u8 *)fb + (u64)row * g_fb.pitch;
                            const u8 *ss = src + (u64)row * stride;
                            u64 n = (u64)copy_w * 4;
                            while (n--) *dd++ = *ss++;
                        }
                    }
                }
            }
        }
        }
        desktop_launch_pending_native();
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
void vscode_launch(void) {
    if (g_vscode_app_id >= 0) {
        launch_linux_app(g_vscode_app_id);
        return;
    }
    /* 未注册 VSCode（CNF 缺失）：仅确保 X server 在线，保持 Phase 3 行为 */
    guest_gui_launch(0, 0, 0);
}

/* attach/detach 时使 scanout 缓存失效并请求全量重绘 */
void ide_attach_invalidate(void) {
    g_so_full_repaint = 1;
    g_so_valid = 0;
}

/* 获取 scanout 状态：优先本帧缓存；缓存无效时查询并缓存。 */
const struct linux_compat_scanout_info *ide_scanout_state(void) {
    if (g_so_valid) return &g_so;
    if (g_lxc_svc && g_lxc_svc->gpu_get_scanout_info &&
        g_lxc_svc->gpu_get_scanout_info(&g_so) == 0) {
        g_so_valid = 1;
        return &g_so;
    }
    return 0;
}

void blit_scanout_to_ide(int hx, int hy, int hw, int hh) {
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
