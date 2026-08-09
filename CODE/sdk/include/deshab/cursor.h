/* deshab/cursor.h - 鼠标光标管理
 *
 * 整合 desktop_app.h 的 da_cursor 结构与 save/restore/draw 操作，统一前缀为 dsb_。
 * 采用 save/restore 模式实现光标移动：绘制前保存背景，移动后恢复。
 */
#ifndef DESHAB_CURSOR_H
#define DESHAB_CURSOR_H

#include "types.h"
#include "app.h"
#include "fb.h"

#define DSB_CURSOR_SIZE 24

static const u8 dsb_cursor_shape[24][24] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

typedef struct dsb_cursor {
    u32 save[DSB_CURSOR_SIZE * DSB_CURSOR_SIZE];
    int saved;
    int old_x, old_y;
    int mx, my;
    int btn;
} dsb_cursor;

static inline void dsb_cursor_init(dsb_cursor *c, i64 fb_w, i64 fb_h) {
    for (int i = 0; i < DSB_CURSOR_SIZE * DSB_CURSOR_SIZE; i++) c->save[i] = 0;
    c->saved = 0;
    c->old_x = -1;
    c->old_y = -1;
    c->mx = (int)(fb_w / 2);
    c->my = (int)(fb_h / 2);
    c->btn = 0;
}

static inline void dsb_cursor_save(dsb_app_context *ac, dsb_cursor *c) {
    for (int r = 0; r < DSB_CURSOR_SIZE; r++)
        for (int col = 0; col < DSB_CURSOR_SIZE; col++) {
            int x = c->mx + col, y = c->my + r;
            c->save[r * DSB_CURSOR_SIZE + col] = dsb_pixel_read(ac, x, y);
        }
    c->saved = 1;
    c->old_x = c->mx;
    c->old_y = c->my;
}

static inline void dsb_cursor_restore(dsb_app_context *ac, dsb_cursor *c) {
    if (!c->saved) return;
    for (int r = 0; r < DSB_CURSOR_SIZE; r++)
        for (int col = 0; col < DSB_CURSOR_SIZE; col++) {
            int x = c->old_x + col, y = c->old_y + r;
            dsb_pixel(ac, x, y, c->save[r * DSB_CURSOR_SIZE + col]);
        }
    c->saved = 0;
}

static inline void dsb_cursor_draw(dsb_app_context *ac, dsb_cursor *c, u32 color) {
    for (int r = 0; r < DSB_CURSOR_SIZE; r++)
        for (int col = 0; col < DSB_CURSOR_SIZE; col++)
            if (dsb_cursor_shape[r][col])
                dsb_pixel(ac, c->mx + col, c->my + r, color);
}

#endif /* DESHAB_CURSOR_H */
