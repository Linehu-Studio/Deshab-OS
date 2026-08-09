/* deshab.h - Deshab SDK 伞头文件
 *
 * 一站式包含 SDK 基础层 + ABI 层 + 应用层轻量头文件。
 * fat32.h 与 net.h 因含较大 static 缓冲（256KB/64KB BSS），
 * 不在此默认包含，按需 #include "deshab/fat32.h" 或 "deshab/net.h"。
 *
 * 用法：
 *   #include "deshab/deshab.h"           // 基础+ABI+应用层
 *   #include "deshab/fat32.h"             // 需要 FAT32 读写时
 *   #include "deshab/net.h"               // 需要网络协议栈时
 *
 * 字体渲染需在 #include "deshab/font.h" 前提供位图数据源
 * （#include "ascii_bitmaps.c" 或自定义 g_ascii/g_ascii_w/g_ascii_h）。
 */
#ifndef DESHAB_H
#define DESHAB_H

/* ---- 基础层 ---- */
#include "types.h"
#include "portio.h"
#include "serial.h"
#include "tsc.h"
#include "string.h"

/* ---- ABI 层 ---- */
#include "kernel_api.h"
#include "boot_context.h"
#include "driver.h"
#include "block.h"

/* ---- 应用层 ---- */
#include "app.h"
#include "fb.h"
#include "cursor.h"
#include "input.h"
#include "ui.h"
#include "rtc.h"
/* font.h 需先提供 g_ascii 位图数据源，不在此默认包含：
 *   #include "ascii_bitmaps.c"
 *   #include "deshab/font.h"
 */

/* ---- 领域层（按需包含，含大 static 缓冲）----
 * #include "fat32.h"
 * #include "net.h"
 */

#endif /* DESHAB_H */
