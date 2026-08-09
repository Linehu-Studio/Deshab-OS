/* deshab/ui.h - 窗口装饰与色板
 *
 * 整合 desktop_app.h 的色板常量与 da_draw_titlebar/draw_statusbar，
 * 以及 LINUXAPP.CNF 解析契约，统一前缀为 dsb_。
 *
 * 独立工具应用全屏运行时的窗口装饰：标题栏 + 关闭按钮 + 状态栏。
 */
#ifndef DESHAB_UI_H
#define DESHAB_UI_H

#include "types.h"
#include "app.h"
#include "fb.h"
#include "font.h"

/* ---- 色板（与 deshab_ui.h DP_* 一致） ---- */
#define DSB_BG_PRIMARY    0xFF0A1428u   /* DP_ABYSS_900 */
#define DSB_BG_SECONDARY  0xFF0F1E38u   /* DP_ABYSS_800 */
#define DSB_BG_TERTIARY   0xFF162848u   /* DP_ABYSS_700 */
#define DSB_TEXT_PRIMARY  0xFFE8E8F0u   /* DP_NEUTRAL_700 */
#define DSB_TEXT_DIM      0xFF6A6A88u   /* DP_NEUTRAL_300 */
#define DSB_ACCENT        0xFF00A8CCu   /* DP_SEAL_500 */
#define DSB_ACCENT_LIGHT  0xFF44CCF0u   /* DP_SEAL_300 */
#define DSB_BORDER        0xFF284888u   /* DP_ABYSS_500 */
#define DSB_BORDER_FOCUS  0xFF44CCF0u   /* DP_SEAL_300 */
#define DSB_CURSOR_COLOR  0xFF00A8CCu   /* DP_SEAL_500 */
#define DSB_PROMPT_COLOR  0xFF44CCF0u   /* DP_SEAL_300 */
#define DSB_SUCCESS       0xFF40C880u   /* DP_SUCCESS */
#define DSB_ERROR         0xFFFF4466u   /* DP_ERROR */
#define DSB_WARNING       0xFFF0A030u   /* DP_WARNING */

/* ---- 窗口装饰尺寸 ---- */
#define DSB_TITLEBAR_H   28
#define DSB_BORDER_W     2
#define DSB_STATUSBAR_H  20

static inline void dsb_draw_titlebar(dsb_app_context *ac, const char *title, i64 w) {
    dsb_fill_rect(ac, 0, 0, w, DSB_TITLEBAR_H, DSB_BG_SECONDARY);
    dsb_draw_string(ac, title, 8, (DSB_TITLEBAR_H - 18) / 2, DSB_TEXT_PRIMARY, DSB_BG_SECONDARY, 12);
    /* 关闭按钮 */
    dsb_fill_rounded_rect(ac, w - 28, 4, 24, 20, DSB_ERROR, 4);
    dsb_draw_string(ac, "X", w - 20, 5, 0xFF0A1428u, DSB_ERROR, 12);
    /* 底部分隔线 */
    dsb_fill_rect(ac, 0, DSB_TITLEBAR_H, w, 1, DSB_ACCENT);
}

static inline void dsb_draw_statusbar(dsb_app_context *ac, const char *text, i64 w, i64 h) {
    i64 y = h - DSB_STATUSBAR_H;
    dsb_fill_rect(ac, 0, y, w, DSB_STATUSBAR_H, DSB_BG_SECONDARY);
    dsb_draw_string(ac, text, 4, y + 1, DSB_TEXT_DIM, DSB_BG_SECONDARY, 12);
    /* 顶部分隔线 */
    dsb_fill_rect(ac, 0, y, w, 1, DSB_BORDER);
}

/* ---- Linux 桌面应用配置契约（LINUXAPP.CNF） ----
 *
 * desktop.elf 启动时从 FAT32 根目录读取 LINUXAPP.CNF（8.3 名 LINUXAPPCNF），
 * 每行注册一个 Linux 应用图标：
 *   显示名|linux命令
 * 示例：
 *   NEOFETCH|/usr/bin/neofetch
 *   HTOP|/usr/bin/htop
 * 规则：空行与 # 开头的行被忽略；显示名 <= 11 字符，命令 <= 63 字符。 */

#define DSB_LINUXAPP_CNF_83   "LINUXAPPCNF"  /* FAT32 8.3 名 */
#define DSB_LINUXAPP_MAX      8
#define DSB_LINUXAPP_NAME_CAP 12
#define DSB_LINUXAPP_CMD_CAP  64

typedef struct {
    char name[DSB_LINUXAPP_NAME_CAP];
    char cmd[DSB_LINUXAPP_CMD_CAP];
} dsb_linux_app;

/* 解析 LINUXAPP.CNF 文本（允许非 NUL 结尾，按 size 截断）。
 * 返回有效条目数（<= max）。 */
static inline int dsb_linuxapp_parse(const char *text, u32 size, dsb_linux_app *out, int max) {
    int count = 0;
    u32 pos = 0;
    while (pos < size && count < max) {
        char line[96];
        int ll = 0;
        while (pos < size && text[pos] != '\n') {
            char c = text[pos++];
            if (c == '\r') continue;
            if (ll < 95) line[ll++] = c;
        }
        if (pos < size && text[pos] == '\n') pos++;
        line[ll] = 0;

        while (ll > 0 && (line[ll - 1] == ' ' || line[ll - 1] == '\t')) line[--ll] = 0;

        int s = 0;
        while (line[s] == ' ' || line[s] == '\t') s++;
        if (line[s] == 0 || line[s] == '#') continue;

        int bar = s;
        while (line[bar] && line[bar] != '|') bar++;
        if (line[bar] != '|') continue;

        int ne = bar;
        while (ne > s && (line[ne - 1] == ' ' || line[ne - 1] == '\t')) ne--;
        if (ne <= s) continue;

        int cs = bar + 1;
        while (line[cs] == ' ' || line[cs] == '\t') cs++;
        if (line[cs] == 0) continue;

        int ni = 0;
        for (int k = s; k < ne && ni < DSB_LINUXAPP_NAME_CAP - 1; k++)
            out[count].name[ni++] = line[k];
        out[count].name[ni] = 0;

        int ci = 0;
        for (int k = cs; line[k] && ci < DSB_LINUXAPP_CMD_CAP - 1; k++)
            out[count].cmd[ci++] = line[k];
        out[count].cmd[ci] = 0;

        count++;
    }
    return count;
}

#endif /* DESHAB_UI_H */
