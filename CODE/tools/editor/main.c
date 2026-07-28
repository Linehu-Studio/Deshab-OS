/* Deshab Editor — 全屏文本编辑器
 * PS/2 键盘输入，行号 + 光标 + 垂直滚动。
 * Ctrl+O 打开文件（输入 8.3 名，从 FAT32 根目录读取）
 * Ctrl+S 保存文件（输入 8.3 名，写入 FAT32 根目录）
 * Esc 退出。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"
#include "../fat32_io.h"

#define CHAR_STEP  12
#define CHAR_H     18
#define EDITOR_BUF_SIZE 65536
#define INPUT_BUF_SIZE  16

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

/* 输入模式：Ctrl+O/Ctrl+S 时进入，在状态栏输入文件名 */
static int  input_mode = 0;       /* 0=编辑, 1=open, 2=save */
static char input_buf[INPUT_BUF_SIZE];
static int  input_len = 0;

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

/* ---- 文件打开/保存 ---- */
static void do_open(void) {
    if (input_len == 0) return;
    char name11[11];
    if (f32_name_to_83(input_buf, name11) != 0) {
        da_slog("editor", "open: bad name");
        return;
    }
    u8 *data = 0; u32 size = 0;
    if (f32_read_root_file(name11, &data, &size) != 0) {
        da_slog("editor", "open: read failed");
        return;
    }
    if (size > EDITOR_BUF_SIZE) size = EDITOR_BUF_SIZE;
    text_len = (int)size;
    for (int i = 0; i < text_len; i++) text_buf[i] = (char)data[i];
    cursor_pos = 0;
    modified = 0;
    /* 更新 filename 显示 */
    int p = 0;
    for (int i = 0; i < input_len && p < 60; i++) filename[p++] = input_buf[i];
    filename[p] = 0;
    recalc_cursor();
    da_slog("editor", "open: ok");
}

static void do_save(void) {
    if (input_len == 0) return;
    char name11[11];
    if (f32_name_to_83(input_buf, name11) != 0) {
        da_slog("editor", "save: bad name");
        return;
    }
    if (f32_write_root_file(name11, (const u8 *)text_buf, (u32)text_len) != 0) {
        da_slog("editor", "save: write failed");
        return;
    }
    modified = 0;
    int p = 0;
    for (int i = 0; i < input_len && p < 60; i++) filename[p++] = input_buf[i];
    filename[p] = 0;
    da_slog("editor", "save: ok");
}

static void enter_input_mode(int mode) {
    input_mode = mode;
    input_len = 0;
    input_buf[0] = 0;
}

static void commit_input(void) {
    int mode = input_mode;
    input_mode = 0;
    if (mode == 1) do_open();
    else if (mode == 2) do_save();
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
            char ln[8];
            ln[0] = '0' + ((line+1)/100)%10;
            ln[1] = '0' + ((line+1)/10)%10;
            ln[2] = '0' + (line+1)%10;
            ln[3] = 0;
            da_draw_string(&g_ac, ln, margin_x, screen_y, DA_TEXT_DIM, DA_BG_SECONDARY, CHAR_STEP);
            da_fill_rect(&g_ac, margin_x + line_num_w - 2, screen_y, 1, CHAR_H, DA_BORDER);
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
            if (line > cursor_line) break;
        }
    }

    /* 光标（编辑模式） */
    if (!input_mode && cursor_line >= scroll_y && cursor_line < scroll_y + visible_rows) {
        int cy = top_y + (cursor_line - scroll_y) * CHAR_H;
        int cx = text_x + cursor_col * CHAR_STEP;
        da_fill_rect(&g_ac, cx, cy, 2, CHAR_H, DA_ACCENT_LIGHT);
    }

    /* 状态栏 */
    char status[96]; p = 0;
    if (input_mode) {
        const char *prompt = (input_mode == 1) ? "Open: " : "Save: ";
        while (*prompt) status[p++] = *prompt++;
        for (int i = 0; i < input_len; i++) status[p++] = input_buf[i];
        status[p++] = '_';
        const char *hint = "  Enter:confirm  Esc:cancel";
        while (*hint && p < 95) status[p++] = *hint++;
    } else {
        const char *s1 = "L:"; while (*s1) status[p++] = *s1++;
        status[p++] = '0' + (cursor_line+1)/10%10; status[p++] = '0' + (cursor_line+1)%10;
        const char *s2 = " C:"; while (*s2) status[p++] = *s2++;
        status[p++] = '0' + (cursor_col+1)/10%10; status[p++] = '0' + (cursor_col+1)%10;
        if (modified) { const char *m = " *"; while (*m) status[p++] = *m++; }
        const char *s3 = "  Ctrl+O:Open  Ctrl+S:Save  Esc:Exit";
        while (*s3 && p < 95) status[p++] = *s3++;
    }
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
    f32_init((f32_block_read_fn)g_ac.block_read, (f32_block_write_fn)g_ac.block_write);
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

        /* Esc：输入模式取消 / 编辑模式退出 */
        if (sc == 0x01 && !e0) {
            if (input_mode) { input_mode = 0; redraw_all(); continue; }
            da_slog("editor", "exit");
            return;
        }

        /* ---- 输入模式：输入文件名 ---- */
        if (input_mode) {
            if (sc == 0x1C) { /* Enter - 确认 */
                commit_input();
                redraw_all();
                continue;
            }
            if (sc == 0x0E) { /* Backspace */
                if (input_len > 0) input_len--;
                input_buf[input_len] = 0;
                redraw_all();
                continue;
            }
            char c = da_scan_to_ascii(sc, shift);
            if (c && c >= 32 && c <= 126 && input_len < INPUT_BUF_SIZE - 1) {
                input_buf[input_len++] = c;
                input_buf[input_len] = 0;
                redraw_all();
            }
            continue;
        }

        /* ---- 编辑模式 ---- */
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
            /* Ctrl+O: 打开文件 */
            if (ctrl_down && (sc == 0x18)) {
                enter_input_mode(1);
                redraw_all();
                continue;
            }
            /* Ctrl+S: 保存文件 */
            if (ctrl_down && (sc == 0x1F)) {
                enter_input_mode(2);
                redraw_all();
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
