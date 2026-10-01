/* ascii_font.c — ASCII 位图字体单副本
 *
 * 经 #define 重命名包含 firstInit/ascii_bitmaps.c（其内部为 static 定义），
 * 再以同名导出指针/值，使所有编译单元共享一份字形数据。
 */
#include "ascii_font.h"

#define g_ascii   g_ascii_static
#define g_ascii_w g_ascii_w_static
#define g_ascii_h g_ascii_h_static
#include "../firstInit/ascii_bitmaps.c"
#undef g_ascii
#undef g_ascii_w
#undef g_ascii_h

const unsigned char (*const g_ascii)[11 * 18] = g_ascii_static;
const int g_ascii_w = g_ascii_w_static;
const int g_ascii_h = g_ascii_h_static;
