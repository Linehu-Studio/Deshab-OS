/* appreg.c — 应用注册表 + 桌面图标（M0 自 main.c 平移）
 *
 * 注册来源：内置 / LINUXAPP.CNF / PEAPPS.CNF / user/desktop/*.lnk
 */
#include "desktop.h"

/* ---- 全局状态 ---- */
app_descriptor g_apps[MAX_APPS];
int g_app_count = 0;

desktop_icon g_icons[MAX_ICONS];
int g_icon_count = 0;

/* ============================================================
 *  Winux-Kate 应用图标（40×40，青色几何图形）
 * ============================================================ */

void draw_circle_outline(du_context *ctx, int cx, int cy, int r, u32 color) {
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

void draw_app_icon(du_context *ctx, int app_id, int cx, int cy) {
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
 *  应用注册
 * ============================================================ */

/* ---- VSCode Phase 5: LINUXAPP.CNF Linux 应用注册表 ----
 * FAT32 根目录 LINUXAPP.CNF（8.3: LINUXAPPCNF），每行一个 Linux guest 应用：
 *   NAME|DISPLAY|/guest/path|args（args 可含空格，可省略）
 * '#' 开头为注释。解析结果存静态池，app_descriptor.linux_path 指入。 */
#define LINUXAPP_MAX 8
static char g_lapp_name[LINUXAPP_MAX][12];
static char g_lapp_disp[LINUXAPP_MAX][16];
static char g_lapp_path[LINUXAPP_MAX][96];
static char g_lapp_args[LINUXAPP_MAX][96];
int g_vscode_app_id = -1;   /* 第一个名为 "vscode" 的注册项 */

/* 字段拷贝：src[0..n) → dst（NUL 结尾），返回拷贝长度 */
static int lapp_field_copy(char *dst, int dst_cap, const char *src, int n) {
    if (n >= dst_cap) n = dst_cap - 1;
    for (int i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
    return n;
}

#define UDESK_MAX 12
static char g_ud_name[UDESK_MAX][12];
static char g_ud_disp[UDESK_MAX][16];
static char g_ud_path[UDESK_MAX][96];
static char g_ud_elf[UDESK_MAX][12];
static char g_ud_files[UDESK_MAX][64];
static int  g_ud_nfiles;

static int udesk_name_taken(const char *n) {
    int i;
    for (i = 0; i < g_app_count; i++) {
        if (g_apps[i].name && fat32_lfn_streq_ci(g_apps[i].name, n)) return 1;
    }
    return 0;
}

static int udesk_collect_cb(const f32_entry *e, void *user) {
    (void)user;
    const char *nm = (e->has_lfn && e->long_name[0]) ? e->long_name : e->name;
    if (e->is_dir || !nm[0]) return 0;
    if (!fat32_lfn_ends_with_ci(nm, ".lnk")) return 0;
    if (g_ud_nfiles >= UDESK_MAX) return 1;
    lapp_field_copy(g_ud_files[g_ud_nfiles], 64, nm, kstrlen(nm));
    g_ud_nfiles++;
    return 0;
}

static int udesk_ascii_ok(const char *s) {
    if (!s || !s[0]) return 0;
    while (*s) {
        if ((unsigned char)*s > 0x7E) return 0;
        s++;
    }
    return 1;
}

static void udesk_parse_lnk(int slot, const u8 *data, u32 size) {
    const char *lines[4];
    int nlen[4];
    int n = 0, i;
    u32 pos = 0;
    char kind[16];
    if (slot < 0 || slot >= UDESK_MAX) return;
    while (pos < size && n < 4) {
        u32 eol = pos;
        while (eol < size && data[eol] != '\n' && data[eol] != '\r') eol++;
        lines[n] = (const char *)data + pos;
        nlen[n] = (int)(eol - pos);
        n++;
        while (eol < size && (data[eol] == '\n' || data[eol] == '\r')) eol++;
        pos = eol;
    }
    if (n < 2) return;
    if (n >= 4 && nlen[3] > 0)
        lapp_field_copy(g_ud_name[slot], 12, lines[3], nlen[3]);
    else
        lapp_field_copy(g_ud_name[slot], 12, lines[0], nlen[0]);
    if (udesk_ascii_ok(g_ud_name[slot]) == 0) {
        g_ud_name[slot][0] = 'a' + (char)slot;
        g_ud_name[slot][1] = 0;
    }
    if (nlen[0] > 0 && udesk_ascii_ok(lines[0]))
        lapp_field_copy(g_ud_disp[slot], 16, lines[0], nlen[0]);
    else
        kstrcpy(g_ud_disp[slot], g_ud_name[slot], 16);
    lapp_field_copy(kind, 16, lines[1], nlen[1]);
    g_ud_path[slot][0] = 0;
    g_ud_elf[slot][0] = 0;
    if (fat32_lfn_streq_ci(kind, "LINUX") || (n >= 3 && lines[2][0] == '/')) {
        if (n >= 3)
            lapp_field_copy(g_ud_path[slot], 96, lines[2], nlen[2]);
    } else if (fat32_lfn_streq_ci(kind, "NATIVE")) {
        return;
    } else {
        if (nlen[1] >= 11)
            lapp_field_copy(g_ud_elf[slot], 12, lines[1], 11);
        else
            f32_name_to_83_w(kind, g_ud_elf[slot]);
        g_ud_elf[slot][11] = 0;
    }
    if (udesk_name_taken(g_ud_name[slot])) return;
    if (!g_ud_path[slot][0] && !g_ud_elf[slot][0]) return;
    if (g_app_count >= MAX_APPS) return;
    g_apps[g_app_count] = (app_descriptor){
        g_ud_name[slot], g_ud_disp[slot], 720, 480,
        0, 0, 0, 0,
        g_ud_elf[slot][0] ? g_ud_elf[slot] : 0,
        g_ud_path[slot][0] ? g_ud_path[slot] : 0, 0, 0, 0,
        1                                    /* user_desktop：桌面只显示 .lnk */
    };
    g_app_count++;
    slog("user/desktop icon:");
    slog(g_ud_disp[slot]);
    (void)i;
}

static void register_user_desktop_icons(void) {
    u32 dclus = 0;
    int i;
    g_ud_nfiles = 0;
    if (f32_find_path_dir_lfn_w("user/desktop", &dclus) != 0) {
        slog("user/desktop missing, skip shared icons");
        return;
    }
    if (f32_list_dir_w(dclus, udesk_collect_cb, 0) != 0) return;
    for (i = 0; i < g_ud_nfiles && g_app_count < MAX_APPS; i++) {
        char path[96];
        u8 *data = 0;
        u32 size = 0;
        kstrcpy(path, "user/desktop/", 96);
        kstrcat(path, g_ud_files[i], 96);
        if (f32_read_path_lfn_w(path, &data, &size) != 0 || !data || !size) continue;
        udesk_parse_lnk(i, data, size);
    }
}

static void register_linux_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83_w("LINUXAPP.CNF", n11) != 0) return;
    if (f32_read_root_file_w(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

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

static void register_pe_apps(void) {
    u8 *data = 0;
    u32 size = 0;
    char n11[11];
    if (f32_name_to_83_w("PEAPPS.CNF", n11) != 0) return;
    if (f32_read_root_file_w(n11, &data, &size) != 0) return;  /* 无配置文件：跳过 */

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

void register_apps(void) {
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
    register_user_desktop_icons();
}

/* M2: 桌面图标 = user/desktop/*.lnk 用户快捷方式（user_desktop 标记）。
 * 内置应用（editor/calc/settings…）不落桌面——经任务栏/开始菜单启动。
 * 布局沿用 canvas padding 24px，列宽 104px，gap 18px，左→右换行。 */
void setup_desktop_icons(void) {
    int pad = KATE_ICON_PAD;                        /* 24 */
    int x = pad;
    int y = g_fluent ? (pad + 8) : (KATE_TOPBAR_H + KATE_PAGE_PAD + pad);
    int max_x = (int)g_fb_w - pad;                  /* canvas 右缘 */
    for (int i = 0; i < g_app_count; i++) {
        if (g_icon_count >= MAX_ICONS) break;
        if (!g_apps[i].user_desktop) continue;      /* M2: 只显示 .lnk 快捷方式 */
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
