/* deshab/boot_context.h - DSK 启动上下文与入口契约
 *
 * 整合 CODE/UTSM/include/utsm/dsk.h，为应用提供自包含的 boot context 定义。
 * DSK 加载 .elf 应用时，通过 dsk_entry(dsk_boot_context *ctx) 传递平台信息。
 *
 * reserved 槽位约定（消除散落的硬编码索引）：
 *   [4] pe_service          PE/EXE 兼容层服务指针
 *   [5] linux_compat_service Linux 兼容层服务指针
 *   [6] probe_info           插桩 probe 缓冲指针
 *   [7] net_lease_info       netman 网络租约指针
 */
#ifndef DESHAB_BOOT_CONTEXT_H
#define DESHAB_BOOT_CONTEXT_H

#include "types.h"

#define DSK_BOOT_MAGIC        0x44534B31424F4F54ULL  /* "DSK1BOOT" */
#define DSK_BOOT_ABI_VERSION  1u

#define DSK_BOOT_FLAG_FROM_UTSM  (1ULL << 0)
#define DSK_BOOT_FLAG_DKM_READY  (1ULL << 1)
#define DSK_BOOT_FLAG_FAT32_PATH (1ULL << 2)
#define DSK_BOOT_FLAG_PROBE_INFO (1ULL << 3)

/* ---- reserved 槽位索引（dsk_boot_context.reserved[]） ---- */
#define DSB_BOOT_RESERVED_PE_SERVICE      4u
#define DSB_BOOT_RESERVED_LINUX_COMPAT    5u
#define DSB_BOOT_RESERVED_PROBE_INFO      6u
#define DSB_BOOT_RESERVED_NET_LEASE       7u

/* ---- Probe 信息结构（通过 reserved[6] 传递） ---- */
#define PROBE_INFO_MAGIC 0x50524F42u  /* "PRFB" */

typedef struct probe_info {
    u32 magic;              /* PROBE_INFO_MAGIC */
    u32 version;            /* ABI 版本，当前 1 */
    u64 probe_buf_addr;     /* probe_entry 缓冲基地址 */
    u32 probe_cap;          /* 缓冲容量（条目数） */
    u32 probe_head;         /* 当前写入位置（递增取模） */
    u32 probe_count;        /* 已写入总条目数 */
    u32 probe_dropped;      /* 因缓冲满而覆盖的条目数 */
    u32 instr_enabled;      /* g_instr_enabled 当前值 */
    u32 instr_probe_enable; /* g_instr_probe_enable 当前值 */
    u64 reserved[4];
} probe_info;

/* ---- 网络租约信息（netman 写入，经 reserved[7] 指针传递） ---- */
#define NET_LEASE_MAGIC   0x4E45544Cu  /* "NETL" */
#define NET_LEASE_F_VALID  (1u << 0)   /* ip/gateway/dns 有效 */
#define NET_LEASE_F_STATIC (1u << 1)   /* 静态配置（非 DHCP） */

typedef struct net_lease_info {
    u32 magic;      /* NET_LEASE_MAGIC */
    u32 flags;      /* NET_LEASE_F_* */
    u32 ip;         /* guest IP（网络序） */
    u32 gateway;    /* 网关（网络序） */
    u32 dns;        /* DNS（网络序） */
    u32 server;     /* DHCP server（网络序，调试） */
    u32 reserved[2];
} net_lease_info;

typedef struct dsk_boot_context {
    u64 magic;
    u32 abi_version;
    u32 size;

    u64 flags;

    u64 hhdm_offset;
    u64 rsdp_address;

    u64 framebuffer_address;
    u64 framebuffer_width;
    u64 framebuffer_height;
    u64 framebuffer_pitch;
    u32 framebuffer_bpp;
    u32 reserved_fb;

    u64 boot_modules_response;

    u64 dkm_kernel_api;
    u64 dkm_driver_table;
    u64 dkm_driver_count;

    u64 utsm_state;
    u64 drr_state;

    u64 memory_map;
    u64 memory_map_count;
    u64 memory_map_entry_size;

    u64 kernel_stack_top;
    u64 reserved[8];
} dsk_boot_context;

/* 应用入口点签名。DSK 加载 .elf 后调用 dsk_entry(context)。 */
typedef void (*dsk_entry_fn)(const dsk_boot_context *context);

#endif /* DESHAB_BOOT_CONTEXT_H */
