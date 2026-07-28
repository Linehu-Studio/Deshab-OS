#ifndef UTSM_PE_H
#define UTSM_PE_H

/* pe.h — Deshab PE/EXE 兼容层公共接口。
 *
 * UTSM 内置 PE32+（64位）与 PE32（32位）加载器：
 *   - PE32+：解析 → 映射节区 → 基址重定位 → import 解析（Windows API shim）→ Ring0 原生执行
 *   - PE32 ：解析 → 映射节区 → 基址重定位 → import 解析 → x86-32 解释器执行
 *
 * pe_service 通过 dsk_boot_context.reserved[4] 暴露给 DSK/cmd.elf，
 * 同地址空间 Ring0 直接调用。
 */

#include <utsm/types.h>

#define PE_SERVICE_MAGIC 0x5045535643200000ULL  /* "PESVC\0\0" */

/* PE 加载结果 */
typedef struct pe_image_info {
    u64 image_base;    /* 实际加载基址（kmem_alloc_aligned 返回） */
    u64 image_size;    /* 映射后总大小（SizeOfImage） */
    u64 entry_point;   /* 绝对入口地址 = image_base + AddressOfEntryPoint */
    int  is_pe32_plus; /* 1=PE32+ 64位原生执行, 0=PE32 32位解释执行 */
    int  load_ok;      /* 1=加载成功 */
} pe_image_info;

/* PE 服务表 — 通过 boot context reserved[4] 传递 */
typedef struct pe_service {
    u64 magic;         /* PE_SERVICE_MAGIC */
    /* 加载 PE 到内存（不执行）。返回 0 成功，info 填充。 */
    int  (*load)(const void *pe_data, u64 size, pe_image_info *out);
    /* 加载并执行 PE。cmdline = 命令行字符串（GetCommandLineA 返回值）。
     * 返回 0 成功，*exit_code 为进程退出码。 */
    int  (*run)(const void *pe_data, u64 size, const char *cmdline, u64 *exit_code);
    /* 释放 pe_load 分配的镜像内存。 */
    void (*unload)(pe_image_info *info);
} pe_service;

/* 获取全局 PE 服务实例指针（UTSM 内部使用，外部通过 boot context 获取） */
const pe_service *pe_get_service(void);

#endif /* UTSM_PE_H */
