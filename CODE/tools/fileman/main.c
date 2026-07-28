/* Deshab File Manager — 全屏文件管理器
 * 接入 fat32_io.h 共享头，支持：
 *   - 子目录遍历（路径栈，Enter 进入 / Backspace 返回）
 *   - FAT 链跟随（多簇文件/目录）
 *   - 文件查看（Enter 打开文本文件，只读浏览，Esc 返回列表）
 *   - 鼠标点击选择 + 键盘导航
 * Esc 退出返回 DSK/desktop。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"
#include "../fat32_io.h"

#define CHAR_STEP  12
#define CHAR_H     18
#define MAX_FILES  256
#define MAX_DEPTH  16
#define VIEW_BUF   24576   /* 文件查看器缓冲 24KB */

static da_app_context g_ac;
static da_cursor g_cursor;
static da_mouse g_mouse;

typedef struct {
    char name[13];
    u32  clus;
    u32  size;
    u8   is_dir;
} fm_entry;

static fm_entry files[MAX_FILES];
static int file_count = 0;
static int selected = 0;
static int scroll_off = 0;

/* 路径栈：每层记录目录首簇（0=根）与显示名 */
static u32  path_clus[MAX_DEPTH];
static char path_name[MAX_DEPTH][13];
static int  path_depth = 0;  /* 0 = 根目录 */

/* 文件查看器状态 */
static int  view_mode = 0;
static u8   view_buf[VIEW_BUF];
static u32  view_size = 0;
static int  view_scroll = 0;

/* ---- 目录列举回调 ---- */
static int fm_collect_cb(const f32_entry *e, void *user_data) {
    (void)user_data;
    if (file_count >= MAX_FILES) return 1;
    fm_entry *f = &files[file_count];
    for (int i = 0; i < 13; i++) f->name[i] = e->name[i];
    f->clus = e->clus;
    f->size = e->size;
    f->is_dir = e->is_dir;
    file_count++;
    return 0;
}

static void load_current_dir(void) {
    file_count = 0;
    selected = 0;
    scroll_off = 0;
    u32 dir_clus = (path_depth == 0) ? 0 : path_clus[path_depth - 1];
    f32_list_dir(dir_clus, fm_collect_cb, 0);
}

/* ---- 路径显示 ---- */
static void build_path_str(char *out, int cap) {
    int p = 0;
    out[p++] = '/';
    for (int i = 0; i < path_depth && p < cap - 13; i++) {
        const char *n = path_name[i];
        while (*n && p < cap - 2) out[p++] = *n++;
        out[p++] = '/';
    }
    out[p] = 0;
}

/* ---- 数字转字符串 ---- */
static void fm_u32_dec(char *buf, u32 v) {
    char tmp[12]; int n = 0;
    if (!v) { buf[0] = '0'; buf[1] = 0; return; }
    while (v && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
}

/* ---- 列表视图绘制 ---- */
static void redraw_list(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);
    da_draw_titlebar(&g_ac, "Files", (i64)g_ac.fb_w);

    /* 当前路径 */
    char path_str[200];
    build_path_str(path_str, sizeof(path_str));
    da_draw_string(&g_ac, path_str, 12, DA_TITLEBAR_H + 4, DA_ACCENT, DA_BG_PRIMARY, CHAR_STEP);

    int margin = 12;
    int top_y = DA_TITLEBAR_H + 4 + CHAR_H + 4;
    int row_h = CHAR_H + 6;
    int visible_rows = ((int)g_ac.fb_h - top_y - DA_STATUSBAR_H - 8) / row_h;
    int col_name_x = margin + 32;
    int col_size_x = (int)g_ac.fb_w - 160;
    int col_type_x = (int)g_ac.fb_w - 60;

    /* 列标题 */
    da_draw_string(&g_ac, "Name", col_name_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_draw_string(&g_ac, "Size", col_size_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_draw_string(&g_ac, "Type", col_type_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_fill_rect(&g_ac, margin, top_y + CHAR_H, (i64)g_ac.fb_w - margin * 2, 1, DA_BORDER);

    /* 滚动范围调整 */
    if (scroll_off < 0) scroll_off = 0;
    if (file_count > visible_rows && scroll_off > file_count - visible_rows)
        scroll_off = file_count - visible_rows;
    if (scroll_off < 0) scroll_off = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = scroll_off + i;
        if (idx >= file_count) break;
        int y = top_y + CHAR_H + 6 + i * row_h;

        if (idx == selected) {
            da_fill_rect(&g_ac, margin, y - 2, (i64)g_ac.fb_w - margin * 2, row_h, DA_BG_TERTIARY);
            da_rect_outline(&g_ac, margin, y - 2, (i64)g_ac.fb_w - margin * 2, row_h, DA_ACCENT, 4);
        }

        u32 icon_color = files[idx].is_dir ? DA_WARNING : DA_ACCENT;
        da_fill_rounded_rect(&g_ac, margin + 4, y, 24, 16, icon_color, 3);

        u32 name_color = (idx == selected) ? DA_ACCENT_LIGHT : DA_TEXT_PRIMARY;
        da_draw_string(&g_ac, files[idx].name, col_name_x, y, name_color,
                       (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);

        if (!files[idx].is_dir) {
            char sz[16];
            fm_u32_dec(sz, files[idx].size);
            int sl = 0; while (sz[sl]) sl++;
            sz[sl++] = ' '; sz[sl++] = 'B'; sz[sl] = 0;
            da_draw_string(&g_ac, sz, col_size_x, y, DA_TEXT_DIM,
                           (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
        } else {
            da_draw_string(&g_ac, "--", col_size_x, y, DA_TEXT_DIM,
                           (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
        }

        const char *type = files[idx].is_dir ? "DIR" : "FILE";
        da_draw_string(&g_ac, type, col_type_x, y, DA_TEXT_DIM,
                       (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
    }

    if (file_count == 0) {
        da_draw_string(&g_ac, "No files found (empty or no block device)",
                       margin + 20, top_y + 60, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    }

    /* 状态栏 */
    char status[80]; int p = 0;
    char nc[12]; fm_u32_dec(nc, (u32)file_count);
    const char *s1 = "items: "; while (*s1) status[p++] = *s1++;
    for (int i = 0; nc[i]; i++) status[p++] = nc[i];
    const char *s2 = "  |  Enter:open  Backspace:up  Esc:exit";
    while (*s2) status[p++] = *s2++;
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
}

/* ---- 文件查看器绘制 ---- */
static void redraw_view(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);

    /* 标题栏显示文件名 */
    char title[32]; int p = 0;
    const char *t = "View: ";
    while (*t && p < 30) title[p++] = *t++;
    if (selected >= 0 && selected < file_count) {
        const char *fn = files[selected].name;
        while (*fn && p < 30) title[p++] = *fn++;
    }
    title[p] = 0;
    da_draw_titlebar(&g_ac, title, (i64)g_ac.fb_w);

    int margin = 12;
    int top_y = DA_TITLEBAR_H + 4;
    int row_h = CHAR_H + 2;
    int visible_rows = ((int)g_ac.fb_h - top_y - DA_STATUSBAR_H - 8) / row_h;

    /* 按行显示文本内容 */
    int y = top_y;
    u32 pos = 0;
    int line = 0;
    /* 跳过 view_scroll 行 */
    int skip = view_scroll;
    while (pos < view_size && skip > 0) {
        if (view_buf[pos] == '\n') { skip--; }
        pos++;
    }
    while (pos < view_size && line < visible_rows) {
        /* 提取一行 */
        int line_start = pos;
        while (pos < view_size && view_buf[pos] != '\n' && pos - line_start < 80) pos++;
        int line_len = pos - line_start;
        /* 绘制字符 */
        for (int i = 0; i < line_len && i < 80; i++) {
            char c = (char)view_buf[line_start + i];
            if (c == '\r') continue;
            if (c < ' ' || c > '~') c = '.';
            char buf[2]; buf[0] = c; buf[1] = 0;
            da_draw_string(&g_ac, buf, margin + i * CHAR_STEP, y, DA_TEXT_PRIMARY, DA_BG_PRIMARY, CHAR_STEP);
        }
        y += row_h;
        line++;
        if (pos < view_size && view_buf[pos] == '\n') pos++;
        /* 处理 \r\n */
        if (pos < view_size && view_buf[pos] == '\r') pos++;
    }

    /* 状态栏 */
    char status[80]; p = 0;
    char sz[12]; fm_u32_dec(sz, view_size);
    const char *s1 = "size: "; while (*s1) status[p++] = *s1++;
    for (int i = 0; sz[i]; i++) status[p++] = sz[i];
    const char *s2 = "B  |  Up/Down:scroll  Esc:back";
    while (*s2) status[p++] = *s2++;
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
}

static void redraw_all(void) {
    if (view_mode) redraw_view();
    else redraw_list();
}

/* ---- 打开文件/目录 ---- */
static void open_selected(void) {
    if (selected < 0 || selected >= file_count) return;
    fm_entry *f = &files[selected];

    if (f->is_dir) {
        /* 进入子目录 */
        if (path_depth < MAX_DEPTH - 1) {
            path_clus[path_depth] = f->clus;
            for (int i = 0; i < 13; i++) path_name[path_depth][i] = f->name[i];
            path_name[path_depth][12] = 0;
            path_depth++;
            load_current_dir();
        }
        return;
    }

    /* 文件：读取内容进查看器 */
    if (f->size == 0) {
        view_size = 0;
    } else {
        u32 rd = f->size;
        if (rd > VIEW_BUF) rd = VIEW_BUF;
        if (f32_read_file_by_clus(f->clus, f->size, view_buf, VIEW_BUF) != 0) {
            da_slog("fileman", "read file failed");
            return;
        }
        view_size = rd;
    }
    view_mode = 1;
    view_scroll = 0;
}

/* ---- 返回上级目录 ---- */
static void go_up(void) {
    if (path_depth > 0) {
        path_depth--;
        load_current_dir();
    }
}

__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("fileman", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("fileman", "bad context");
        for(;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    da_cursor_init(&g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h);
    da_mouse_init();

    /* 初始化 fat32_io：注入 block_read/block_write */
    f32_init((f32_block_read_fn)g_ac.block_read, (f32_block_write_fn)g_ac.block_write);

    path_depth = 0;
    load_current_dir();
    redraw_all();

    int e0 = 0, shift = 0; (void)shift;
    for (;;) {
        int need_redraw = 0;

        /* 鼠标轮询 */
        if (da_mouse_poll(&g_mouse, &g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h)) {
            need_redraw = 1;
            if (g_cursor.btn && !view_mode) {
                int top_y = DA_TITLEBAR_H + 4 + CHAR_H + 4;
                int row_h = CHAR_H + 6;
                int click_y = g_cursor.my - top_y - CHAR_H - 6;
                if (click_y >= 0) {
                    int idx = click_y / row_h + scroll_off;
                    if (idx >= 0 && idx < file_count) {
                        selected = idx;
                        need_redraw = 1;
                    }
                }
            }
        }

        /* 键盘轮询 */
        u8 st = inb(0x64);
        if ((st & 1) && !(st & 0x20)) {
            u8 data = inb(0x60);
            u8 sc = data;
            if (sc == 0xE0) { e0 = 1; goto skip; }
            if (sc == 0x2A || sc == 0x36) { shift = 1; goto skip; }
            if (sc == 0xAA || sc == 0xB6) { shift = 0; goto skip; }
            if (sc & 0x80) { e0 = 0; goto skip; }

            if (sc == 0x01) { /* Esc */
                if (view_mode) {
                    view_mode = 0;
                    need_redraw = 1;
                } else {
                    da_slog("fileman", "exit");
                    return;
                }
                goto skip;
            }

            if (view_mode) {
                /* 查看器模式：Up/Down 滚动 */
                if (e0) {
                    if (sc == 0x48) { if (view_scroll > 0) view_scroll--; need_redraw = 1; }
                    else if (sc == 0x50) { view_scroll++; need_redraw = 1; }
                    e0 = 0;
                    goto skip;
                }
                /* PageUp/PageDown */
                if (sc == 0x49) { view_scroll -= 10; if (view_scroll < 0) view_scroll = 0; need_redraw = 1; }
                else if (sc == 0x51) { view_scroll += 10; need_redraw = 1; }
                goto skip;
            }

            /* 列表模式 */
            if (e0) {
                if (sc == 0x48) { /* Up */
                    if (selected > 0) selected--;
                    if (selected < scroll_off) scroll_off = selected;
                    need_redraw = 1;
                } else if (sc == 0x50) { /* Down */
                    if (selected < file_count - 1) selected++;
                    need_redraw = 1;
                }
                e0 = 0;
                goto skip;
            }

            if (sc == 0x1C) { /* Enter - 打开 */
                open_selected();
                need_redraw = 1;
            } else if (sc == 0x0E) { /* Backspace - 返回上级 */
                go_up();
                need_redraw = 1;
            } else if (sc == 0x49) { /* PageUp */
                selected -= 10; if (selected < 0) selected = 0;
                if (selected < scroll_off) scroll_off = selected;
                need_redraw = 1;
            } else if (sc == 0x51) { /* PageDown */
                selected += 10; if (selected >= file_count) selected = file_count - 1;
                need_redraw = 1;
            }
        }
    skip:
        if (need_redraw) {
            da_cursor_restore(&g_ac, &g_cursor);
            redraw_all();
        }
        __asm__("pause");
    }
}
