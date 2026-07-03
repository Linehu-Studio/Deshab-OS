/* Deshab File Manager — 全屏文件管理器
 * 文件列表视图，鼠标点击选择，键盘导航。
 * FAT32 根目录遍历，Esc 退出。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"

#define CHAR_STEP  12
#define CHAR_H     18
#define MAX_FILES  256

static da_app_context g_ac;
static da_cursor g_cursor;
static da_mouse g_mouse;

typedef struct {
    char name[13]; /* 8.3 */
    int  is_dir;
    u32  size;
} file_entry;

static file_entry files[MAX_FILES];
static int file_count = 0;
static int selected = 0;
static int scroll_off = 0;

static void load_root_dir(void) {
    file_count = 0;
    if (!g_ac.block_read) return;

    u8 bpb[512];
    if (g_ac.block_read(0, 0, 1, bpb) != 0) return;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) return;

    u32 root_cluster = *(u32 *)(bpb + 44);
    u8  sectors_per_cluster = bpb[13];
    u32 fat_start = *(u16 *)(bpb + 14);
    u32 data_start = fat_start + (*(u16 *)(bpb + 22)) * 2;

    u8 dir_buf[8192];
    u32 lba = data_start + (root_cluster - 2) * sectors_per_cluster;
    if (g_ac.block_read(0, lba, sectors_per_cluster, dir_buf) != 0) return;

    for (int i = 0; i < (int)(sectors_per_cluster * 512) && file_count < MAX_FILES; i += 32) {
        u8 first = dir_buf[i];
        if (first == 0x00) break;
        if (first == 0xE5) continue;
        if (dir_buf[i+11] & 0x08) continue;
        if (dir_buf[i+11] & 0x10) continue; /* 暂跳过目录 */

        file_entry *f = &files[file_count];
        f->is_dir = (dir_buf[i+11] & 0x10) ? 1 : 0;
        f->size = *(u32 *)(dir_buf + i + 28);
        int p = 0;
        for (int j = 0; j < 8 && dir_buf[i+j] != ' '; j++)
            f->name[p++] = (char)dir_buf[i+j];
        if (dir_buf[i+8] != ' ') {
            f->name[p++] = '.';
            for (int j = 8; j < 11 && dir_buf[i+j] != ' '; j++)
                f->name[p++] = (char)dir_buf[i+j];
        }
        f->name[p] = 0;
        file_count++;
    }
}

static void redraw_all(void) {
    da_fill_bg(&g_ac, DA_BG_PRIMARY);
    da_draw_titlebar(&g_ac, "Files", (i64)g_ac.fb_w);

    int margin = 12;
    int top_y = DA_TITLEBAR_H + 4;
    int row_h = CHAR_H + 6;
    int visible_rows = ((int)g_ac.fb_h - top_y - DA_STATUSBAR_H - 8) / row_h;
    int col_name_x = margin + 32;
    int col_size_x = (int)g_ac.fb_w - 120;
    int col_type_x = (int)g_ac.fb_w - 60;

    /* 列标题 */
    da_draw_string(&g_ac, "Name", col_name_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_draw_string(&g_ac, "Size", col_size_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_draw_string(&g_ac, "Type", col_type_x, top_y, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    da_fill_rect(&g_ac, margin, top_y + CHAR_H, (i64)g_ac.fb_w - margin * 2, 1, DA_BORDER);

    /* 文件列表 */
    if (scroll_off < 0) scroll_off = 0;
    if (scroll_off > file_count - visible_rows && file_count > visible_rows) scroll_off = file_count - visible_rows;
    if (scroll_off < 0) scroll_off = 0;

    for (int i = 0; i < visible_rows; i++) {
        int idx = scroll_off + i;
        if (idx >= file_count) break;
        int y = top_y + CHAR_H + 6 + i * row_h;

        /* 选中高亮 */
        if (idx == selected) {
            da_fill_rect(&g_ac, margin, y - 2, (i64)g_ac.fb_w - margin * 2, row_h, DA_BG_TERTIARY);
            da_rect_outline(&g_ac, margin, y - 2, (i64)g_ac.fb_w - margin * 2, row_h, DA_ACCENT, 4);
        }

        /* 图标 */
        u32 icon_color = files[idx].is_dir ? DA_WARNING : DA_ACCENT;
        da_fill_rounded_rect(&g_ac, margin + 4, y, 24, 16, icon_color, 3);

        /* 文件名 */
        u32 name_color = (idx == selected) ? DA_ACCENT_LIGHT : DA_TEXT_PRIMARY;
        da_draw_string(&g_ac, files[idx].name, col_name_x, y, name_color,
                       (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);

        /* 大小 */
        if (!files[idx].is_dir) {
            char sz[16]; int p = 0;
            u32 s = files[idx].size;
            if (s == 0) { sz[p++] = '0'; }
            else { char tmp[12]; int tl = 0; while(s>0){tmp[tl++]='0'+(s%10);s/=10;} for(int j=tl-1;j>=0;j--)sz[p++]=tmp[j]; }
            sz[p++] = ' '; sz[p++] = 'B'; sz[p] = 0;
            da_draw_string(&g_ac, sz, col_size_x, y, DA_TEXT_DIM,
                           (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
        } else {
            da_draw_string(&g_ac, "--", col_size_x, y, DA_TEXT_DIM,
                           (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
        }

        /* 类型 */
        const char *type = files[idx].is_dir ? "DIR" : "FILE";
        da_draw_string(&g_ac, type, col_type_x, y, DA_TEXT_DIM,
                       (idx == selected) ? DA_BG_TERTIARY : DA_BG_PRIMARY, CHAR_STEP);
    }

    if (file_count == 0) {
        da_draw_string(&g_ac, "No files found (empty or no block device)",
                       margin + 20, top_y + 60, DA_TEXT_DIM, DA_BG_PRIMARY, CHAR_STEP);
    }

    /* 状态栏 */
    char status[64]; int p = 0;
    const char *p1 = "/  "; while (*p1) status[p++] = *p1++;
    status[p++] = '0' + file_count / 10; status[p++] = '0' + file_count % 10;
    const char *p2 = " items"; while (*p2) status[p++] = *p2++;
    status[p] = 0;
    da_draw_statusbar(&g_ac, status, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    /* 鼠标光标 */
    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
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

    load_root_dir();
    redraw_all();

    int e0 = 0, shift = 0; (void)shift;
    for (;;) {
        int need_redraw = 0;

        /* 鼠标轮询 */
        if (da_mouse_poll(&g_mouse, &g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h)) {
            need_redraw = 1;
            if (g_cursor.btn) {
                /* 点击选择文件 */
                int margin = 12;
                int top_y = DA_TITLEBAR_H + 4;
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

            if (sc == 0x01) { da_slog("fileman", "exit"); return; }

            if (e0) {
                if (sc == 0x48) { /* Up */
                    if (selected > 0) selected--;
                    need_redraw = 1;
                } else if (sc == 0x50) { /* Down */
                    if (selected < file_count - 1) selected++;
                    need_redraw = 1;
                }
                e0 = 0;
                goto skip;
            }

            if (sc == 0x1C) { /* Enter - 打开 */
                if (selected >= 0 && selected < file_count) {
                    da_slog("fileman", "open file (stub)");
                }
                need_redraw = 1;
            } else if (sc == 0x0E) { /* Backspace - 返回上级 */
                da_slog("fileman", "back (root only)");
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
