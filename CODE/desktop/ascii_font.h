/* ascii_font.h — Consolas 18px ASCII 位图字体（全程序单副本导出）
 *
 * 背景：ascii_bitmaps.c 以 static 数组定义 g_ascii（含它的编译单元各持一份
 * ~19KB 副本）。多模块拆分后由 ascii_font.c 经 #define 重命名技巧包含
 * ascii_bitmaps.c，导出唯一一份；deshab_ui.h 的 du_draw_char/du_draw_string
 * 引用这些 extern 符号。
 *
 * 用法：#include "ascii_font.h" 之后 #include <utsm/deshab_ui.h>。
 * firstInit 等旧代码仍直接 include ascii_bitmaps.c，不受影响。
 */
#ifndef DESHAB_ASCII_FONT_H
#define DESHAB_ASCII_FONT_H

extern const unsigned char (*const g_ascii)[11 * 18];
extern const int g_ascii_w;
extern const int g_ascii_h;

#endif /* DESHAB_ASCII_FONT_H */
