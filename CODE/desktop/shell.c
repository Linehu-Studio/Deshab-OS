/* shell.c — 工作区/页面状态机 + 任务视图/右键菜单 + 事件路由（M0 自 main.c 平移）
 */
#include "desktop.h"

/* ---- 全局状态 ---- */
int g_page = 1;                 /* 1..3 内置, 4..8 自定义 */
int g_taskview = 0;
int g_dash_focus = 0;           /* 0=t1 1=t2 2=ed 3=files */

workspace g_ws[MAX_WS];
int g_ws_count = 1;
int g_ws_cur = 0;

cpage g_cpages[MAX_CPAGES];
int g_cpage_count = 0;

/* 右键菜单（移到工作区） */
int g_ctx_open = 0;
int g_ctx_x = 0, g_ctx_y = 0;
int g_ctx_win = -1;

/* 状态栏滑块 */
int g_vol = 65;
int g_bright = 100;
int g_bright_applied = 100;  /* 上次应用的亮度值（避免重复处理） */
int g_drag_slider = 0;          /* 1=vol 2=bright */
int g_vol_rect[4];              /* x,y,w,h 滑轨 */
int g_bri_rect[4];

int g_quit = 0;                 /* EXIT 按钮 → 返回 DSK */

/* 双击检测 */
u64 g_last_click_tsc = 0;
int g_last_click_x = -100, g_last_click_y = -100;

/* 面板/控件命中矩形（绘制时更新，点击时使用） */
int g_dash_rect[4][4];          /* 4 面板 x,y,w,h */
int g_save_rect[4];
int g_pg_rect[8][4];            /* 页面切换按钮 */
int g_pg_rm_rect[8][4];
int g_pgadd_rect[4];
int g_tvbtn_rect[4];            /* 任务视图按钮 */
int g_ws_rect[MAX_WS][4];
int g_wsadd_rect[4];
int g_tb_rect[MAX_WINDOWS][4];  /* 任务栏项 */
int g_tb_close_rect[MAX_WINDOWS][4];
int g_tb_quit_rect[4];
int g_tb_adopt_rect[4];           /* 收纳窗口按钮 */
int g_tv_card[MAX_WS + 1][4];   /* 任务视图卡片(+add) */
int g_tv_close[MAX_WS][4];
int g_ctx_rect[4];
int g_ctx_item[MAX_WS + 1][4];
int g_ctx_item_ws[MAX_WS + 1];   /* 菜单行 → 真实工作区索引，-1=新建 */
int g_ide_host[4];
int g_ide_tabx[4];              /* IDE 标签 × */
int g_ide_new_rect[4];          /* IDE "+ NEW IDE" 按钮 */
int g_cp_host[4];               /* 自定义页宿主 */
int g_cp_new_rect[4];           /* 自定义页 "+ NEW INSTANCE" 按钮 */

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

void page_switch(int p) {
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
            if (f32_neq11_w(g_cpages[i].elf11, g_apps[a].elf_name)) { dup = 1; break; }
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

void slider_set(const int rect[4], int *val, int mx) {
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

/* 构建"移到工作区"列表（排除当前工作区，与 Winux-Kate 一致），返回行数 */
int ctx_list_build(int *map) {
    int n = 0;
    for (int i = 0; i < g_ws_count; i++) {
        if (i == g_ws_cur) continue;
        map[n++] = i;
    }
    return n;
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
    /* M2 Fluent：悬浮任务栏 + 托盘优先路由 */
    if (g_fluent && taskbar_click(mx, my)) return 1;

    int tb_y = (int)g_fb_h - (g_fluent ? 56 : KATE_TASKBAR_H);

    /* 任务栏（仅 KATE 回退路径；Fluent 已由 taskbar_click 消费） */
    if (!g_fluent && my >= tb_y) {
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
        /* M2 Fluent：右上三钮（关闭/最大化/最小化） */
        if (g_fluent) {
            if (win_hit_close_btn(w, mx, my)) { win_close_request(w); return 1; }
            if (win_hit_max_btn(w, mx, my)) {
                /* 最大化/还原：铺满桌面区（顶部 0 到任务栏上方） */
                if (w->w >= (int)g_fb_w - 8 && w->h >= (int)g_fb_h - 64) {
                    w->x = 60; w->y = 40; w->w = 720; w->h = 500;   /* 还原 */
                } else {
                    w->x = 0; w->y = 0;
                    w->w = (int)g_fb_w;
                    w->h = (int)g_fb_h - 56;   /* 任务栏悬浮区预留 */
                }
                return 1;
            }
            if (win_hit_min_btn(w, mx, my)) {
                w->minimized = 1;
                w->focused = 0;
                g_focused_win = -1;
                return 1;
            }
        } else if (win_hit_close_btn(w, mx, my)) { win_close_request(w); return 1; }
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
int on_left_press(void) {
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

    /* 顶栏（仅 KATE 回退路径；Fluent 无 topbar，点击直达页面内容） */
    if (!g_fluent && my < KATE_TOPBAR_H) {
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
int on_right_press(void) {
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
int handle_key(u8 sc) {
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
        /* IDE 页：Esc = detach + 退回桌面页（IDE 退出通道） */
        if (g_page == 2) {
            if (g_ide_attached) {
                g_ide_attached = 0;
                g_input_forward_enabled = 0;
                ide_attach_invalidate();
                slog("ide detached (esc), input forwarding off");
            }
            page_switch(3);
            return 1;
        }
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
