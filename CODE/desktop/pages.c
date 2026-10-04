/* pages.c — KATE 风格页面绘制（M0 自 main.c 平移）
 *
 * 包含：topbar / DASHBOARD / StatusBar / IDE / DESKTOP(含旧任务栏) /
 * CUSTOM / 任务视图 / 右键菜单。M2-M4 将按 Fluent 计划逐页替换：
 * topbar→删除、DASHBOARD→小组件、DESKTOP 任务栏→悬浮任务栏。
 */
#include "desktop.h"
#include "zhfont.h"       /* M2: 桌面图标中文标签 */

/* ============================================================
 *  顶栏（brand + WorkspaceSwitcher + PageSwitcher + PG 时钟）
 * ============================================================ */

void draw_topbar(void) {
    int W = (int)g_fb_w;
    du_fill_rect_gradient(&g_fb, 0, 0, W, KATE_TOPBAR_H,
                          0xFFFFFFFFu, KS_BG_PRIMARY);
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
    du_fill_rect(&g_fb, 0, y, W, KATE_STATUSBAR_H, KS_BG_SECONDARY);
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
        zh_draw_string(&g_fb, "桌面无快捷方式",
                       KATE_ICON_PAD, KATE_TOPBAR_H + KATE_PAGE_PAD + KATE_ICON_PAD + 16,
                       KS_TEXT_DIM, 0);
        return;
    }
    for (int i = 0; i < g_icon_count; i++) {
        desktop_icon *ic = &g_icons[i];
        if (g_fluent) {
            /* M2 Fluent：图标 + 中文标签（zh 字库，缺失自动回退 ASCII） */
            du_kate_desktop_icon(&g_fb, ic->x, ic->y, "", ic->selected);
            int ix = ic->x + (KATE_ICON_W - KATE_ICON_IMG) / 2;
            int iy = ic->y + 10;
            draw_app_icon(&g_fb, ic->app_id, ix, iy);
            int lw = zh_string_width(ic->label, 0);
            zh_draw_string(&g_fb, ic->label,
                           ic->x + (KATE_ICON_W - lw) / 2,
                           ic->y + KATE_ICON_IMG + 16,
                           ic->selected ? 0xFFFFFFFFu : 0xFFE8E8E8u, 0);
        } else {
            du_kate_desktop_icon(&g_fb, ic->x, ic->y, ic->label, ic->selected);
            int ix = ic->x + (KATE_ICON_W - KATE_ICON_IMG) / 2;
            int iy = ic->y + 10;
            draw_app_icon(&g_fb, ic->app_id, ix, iy);
        }
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

void draw_dashboard(void) {
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
void draw_ide(void) {
    int W = (int)g_fb_w;
    int top = KATE_TOPBAR_H;

    /* ide-tabs（36px）：dot + 实例标签 + "+ NEW IDE" + ide-hint（Kate 结构） */
    du_fill_rect(&g_fb, 0, top, W, 36, KS_BG_SECONDARY);
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
                du_fill_rect(&g_fb, hx + 2, hy + 2, hw - 4, hh - 4, KS_BG_SECONDARY);
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
void draw_desktop_page(void) {
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

        if (!g_fluent) {   /* KATE 扫描线特效；Fluent 窗口干净背景 */
            du_kate_scanlines(&g_fb, w->x + 1, w->y + KATE_TITLEBAR_H,
                              w->w - 2, w->h - KATE_TITLEBAR_H - 1);
        }
    }

    /* M2 Fluent：悬浮任务栏由 compositor（redraw_all_fluent）统一叠加，
     * 此处跳过旧 40px 任务栏。 */
    if (g_fluent) return;

    /* 任务栏（40px，KATE 回退路径） */
    int W = (int)g_fb_w;
    int tb_y = (int)g_fb_h - KATE_TASKBAR_H;
    du_fill_rect_gradient(&g_fb, 0, tb_y, W, KATE_TASKBAR_H,
                          0xFFFFFFFFu, KS_BG_PRIMARY);
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
void draw_custom_page(int cp_idx) {
    int W = (int)g_fb_w;
    int top = KATE_TOPBAR_H;
    cpage *cp = &g_cpages[cp_idx];

    /* ide-tabs 栏（36px）：dot + 页名 + "+ NEW INSTANCE" + ide-hint（Kate 结构） */
    du_fill_rect(&g_fb, 0, top, W, 36, KS_BG_SECONDARY);
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
        f32_name_from_83_w(cp->elf11, disp);
        kstrcat(sub, disp, 40);
    }
    draw_centered(sub, hx + hw / 2, hy + hh / 2 + 12, KS_ACCENT, 0);
}

/* ============================================================
 *  任务视图 overlay
 * ============================================================ */

void draw_taskview(void) {
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

void draw_ctx_menu(void) {
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

    du_fill_rect(&g_fb, x, y, mw, mh, KS_BG_SECONDARY);
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
