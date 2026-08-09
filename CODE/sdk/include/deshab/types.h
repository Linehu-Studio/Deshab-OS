/* deshab/types.h - Deshab SDK 基础类型定义
 *
 * 单一来源：所有 SDK 头文件与应用/驱动均通过本头获取基础类型，
 * 消除 dkm_shared.h / desktop_app.h / net_stack.h 各自重复定义的碎片化。
 * 布局与 CODE/UTSM/include/utsm/types.h 保持一致。
 */
#ifndef DESHAB_TYPES_H
#define DESHAB_TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         i8;
typedef short               i16;
typedef int                 i32;
typedef long long           i64;

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* DESHAB_TYPES_H */
