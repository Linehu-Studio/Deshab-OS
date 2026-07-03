/* Deshab Editor — 全屏文本编辑器
 * PS/2 键盘输入，行号 + 光标 + 垂直滚动。
 * Ctrl+O 打开文件，Ctrl+S 保存（暂不支持），Esc 退出。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"

#define CHAR_STEP  12
#define CHAR_H     18
#define EDITOR_BUF_SIZE 65536

static da_app_context g_ac;

static char text_buf[EDITOR_BUF_SIZE];
static int text_len = 0;
static int cursor_pos = 0;
static int scroll_y = 0;
static int cursor_line = 0;
static int cursor_col = 0;
static int modified = 0;
static char filename[64] = "untitled";
static int ctrl_down = 0;

static int line_start(int line) {
    int l = 0;
    for (int i = 0; i < text_len; i++) {
        if (l == line) return i;
        if (text_buf[i] == '\n') l++;
    }
    return text_len;
}

static void recalc_cursor(void) {
    cursor_line = 0; cursor_col = 0;
    for (int i = 0; i < cursor_pos && i < text_len; i++) {
        if (text_buf[i] == '\n') { cursor_line++; cursor_col = 0; }
        else cursor_col++;
    }
}

static void init_default_text(void) {
    const char *sample = "# Deshab Editor\n#\n\nStart typing here...\n\nCtrl+O: Open  Ctrl+S: Save  Esc: Exit\n";
    while (*sample && text_len < EDITOR_BUF_SIZE - 1)
        text_buf[text_len++] = *sample++;
    cursor_pos = text_len;
    recalc_cursor();
}

static void redraw_all(void) {
    da_fill_bg(&g_ac, DA_BG_SECONDARY);
    char title[80];
    int p = 0;
    const char *t1 = "Editor - ";
    while (*t1) title[p++] = *t1++;
    for (int i = 0; filename[i] && p < 70; i++) title[p++] = filename[i];
    if (modified) { title[p++] = ' '; title[p++] = '*'; }
    title[p] = 0;
    da_draw_titlebar(&g_ac, title, (i64)g_ac.fb_w);

    int margin_x = 8;
    int top_y = DA_TITLEBAR_H + 4;
    int line_num_w = 5 * CHAR_STEP + 8;
    int text_x = margin_x + line_num_w;
    int visible_rows = ((int)g_ac.fb_h - top_y - DA_STATUSBAR_H - 8) / CHAR_H;
    int visible_cols = ((int)g_ac.fb_w - text_x - 8) / CHAR_STEP;

    /* 自动滚动 */
    if (cursor_line < scroll_y) scroll_y = cursor_line;
    if (cursor_line >= scroll_y + visible_rows) scroll_y = cursor_line - visible_rows + 1;

    /* 渲染可见行 */
    int line = 0, pos = 0;
    for (; pos <= text_len && line < scroll_y + visible_rows; ) {
        if (line >= scroll_y) {
            int screen_y = top_y + (line - scroll_y) * CHAR_H;
            /* 行号 */
            char ln[8];
            ln[0] = '0' + ((line+1)/100)%10;
            ln[1] = '0' + ((line+1)/10)%10;
            ln[2] = '0' + (line+1)%10;
            ln[3] = 0;
            da_draw_string(&g_ac, ln, margin_x, screen_y, DA_TEXT_DIM, DA_BG_SECONDARY, CHAR_STEP);
            /* 分隔线 */
            da_fill_rect(&g_ac, margin_x + line_num_w - 2, screen_y, 1, CHAR_H, DA_BORDER);
            /* 文本 */
            int col = 0;
            while (pos < text_len && text_buf[pos] != '\n' && col < visible_cols) {
                da_draw_char(&g_ac, text_buf[pos],
                             text_x + col * CHAR_STEP, screen_y,
                             DA_TEXT_PRIMARY, DA_BG_SECONDARY);
                col++; pos++;
            }
        } else {
            while (pos < text_len && text_buf[pos] != '\n') pos++;
        }
        if (pos < text_len && text_buf[pos] == '\n') pos++;
        line++;
        if (pos >= text_len) {
            /* 空行也画光标 */
            if (line > cursor_line) break;
        }
    }

    /* 光标 */
    if (cursor_line >= scroll_y && cursor_line < scroll_y + visible_rows) {
        int cy = top_y + (cursor_line - scroll_y) * CHAR_H;
        int cx = text_x + cursor_col * CHAR_STEP;
        da_fill_rect(&g_ac, cx, cy, 2, CHAR_H, DA_ACCENT_LIGHT);
    }

    /* 状态栏 */
    char status[64]; p = 0;
    const char *s1 = "L:"; while (*s1) status[p++] = *s1++;
    status[p++] = '0' + (cursor_line+1)/10%10; status[p++] = '0' + (cursor_line+1)%10;
    const char *s2 = " C:"; while (*s2) status[p++] = *s2++;
    status[p++] = '0' + (cursor_col+1)/10%10; status[p++] = '0' + (cursor_col+1)%10;
    if (modified) { const char *m = " *"; while (*m) status[p++] = *m++; }
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
}

__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("editor", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("editor", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    init_default_text();
    redraw_all();

    int shift = 0, e0 = 0;
    for (;;) {
        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        u8 data = inb(0x60);
        if (st & 0x20) continue;
        u8 sc = data;
        if (sc == 0xE0) { e0 = 1; continue; }
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc == 0x1D) { ctrl_down = 1; continue; }
        if (sc == 0x9D) { ctrl_down = 0; continue; }
        if (sc & 0x80) { e0 = 0; continue; }

        /* Esc 退出 */
        if (sc == 0x01 && !e0) { da_slog("editor", "exit"); return; }

        if (e0) {
            if (sc == 0x48) { /* Up */
                if (cursor_line > 0) {
                    int prev = line_start(cursor_line - 1);
                    cursor_pos = prev;
                    for (int i = 0; i < cursor_col && cursor_pos < text_len && text_buf[cursor_pos] != '\n'; i++)
                        cursor_pos++;
                }
            } else if (sc == 0x50) { /* Down */
                int next = line_start(cursor_line + 1);
                cursor_pos = next;
                for (int i = 0; i < cursor_col && cursor_pos < text_len && text_buf[cursor_pos] != '\n'; i++)
                    cursor_pos++;
            } else if (sc == 0x4B) { /* Left */
                if (cursor_pos > 0) cursor_pos--;
            } else if (sc == 0x4D) { /* Right */
                if (cursor_pos < text_len) cursor_pos++;
            } else if (sc == 0x47) { /* Home */
                cursor_pos = line_start(cursor_line);
            } else if (sc == 0x4F) { /* End */
                cursor_pos = line_start(cursor_line);
                while (cursor_pos < text_len && text_buf[cursor_pos] != '\n') cursor_pos++;
            } else if (sc == 0x49) { /* PageUp */
                for (int i = 0; i < 10 && cursor_line > 0; i++) {
                    cursor_pos = line_start(cursor_line - 1);
                    recalc_cursor();
                }
            } else if (sc == 0x51) { /* PageDown */
                for (int i = 0; i < 10; i++) {
                    int next = line_start(cursor_line + 1);
                    if (next >= text_len) break;
                    cursor_pos = next;
                    recalc_cursor();
                }
            }
            e0 = 0;
            recalc_cursor();
            redraw_all();
            continue;
        }

        if (sc == 0x1C) { /* Enter */
            if (text_len < EDITOR_BUF_SIZE - 1) {
                for (int i = text_len; i > cursor_pos; i--) text_buf[i] = text_buf[i-1];
                text_buf[cursor_pos] = '\n';
                text_len++; cursor_pos++;
                modified = 1;
            }
        } else if (sc == 0x0E) { /* Backspace */
            if (cursor_pos > 0) {
                for (int i = cursor_pos - 1; i < text_len - 1; i++) text_buf[i] = text_buf[i+1];
                text_len--; cursor_pos--;
                modified = 1;
            }
        } else if (sc == 0x53 && e0) { /* Delete */
            if (cursor_pos < text_len) {
                for (int i = cursor_pos; i < text_len - 1; i++) text_buf[i] = text_buf[i+1];
                text_len--;
                modified = 1;
            }
        } else {
            /* Ctrl+O: 打开文件（提示暂不支持） */
            if (ctrl_down && (sc == 0x18)) {
                /* Ctrl+O - stub */
                da_slog("editor", "Ctrl+O - open not yet");
                continue;
            }
            /* Ctrl+S: 保存（提示暂不支持） */
            if (ctrl_down && (sc == 0x1F)) {
                da_slog("editor", "Ctrl+S - save not yet");
                modified = 0;
                continue;
            }
            char c = da_scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126 && text_len < EDITOR_BUF_SIZE - 1) {
                for (int i = text_len; i > cursor_pos; i--) text_buf[i] = text_buf[i-1];
                text_buf[cursor_pos] = c;
                text_len++; cursor_pos++;
                modified = 1;
            }
        }
        recalc_cursor();
        redraw_all();
    }
}
