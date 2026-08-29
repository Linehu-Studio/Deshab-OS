/* deshab/kernel_api.h - dkm_kernel_api ABI 单一定义来源
 *
 * 整合 CODE/UTSM/include/utsm/dkm.h 与 CODE/DKM/dkm_shared.h 的 ABI 结构体，
 * 消除驱动本地重复定义 dkm_kernel_api 的隐患（CONTRIBUTING.md 硬约束）。
 *
 * 约定：
 *   - 字段顺序/类型不可调整，UTSM 端按固定偏移填充。
 *   - 新增字段只允许追加在结构末尾（尾部追加保持 ABI 兼容）。
 *   - 结构体同时提供 typedef 名与 struct 标签，兼容两种引用方式。
 *
 * 偏移常量（供不便解引用 kernel_api 指针的场景使用，消除硬编码）：
 *   DSB_KAPI_OFF_NET    0x48   net API 指针
 *   DSB_KAPI_OFF_BLOCK  0xA8   block API 指针
 */
#ifndef DESHAB_KERNEL_API_H
#define DESHAB_KERNEL_API_H

#include "types.h"

#define DKM_KERNEL_API_VERSION 1u

/* ---- 网卡链路/能力标志 ---- */
#define DKM_NET_F_LINK_UP  (1u << 0)
#define DKM_NET_F_TX_READY (1u << 1)
#define DKM_NET_F_RX_READY (1u << 2)
#define DKM_NET_F_WIRELESS (1u << 3)

/* ---- WiFi 扫描加密类型 ---- */
#define DKM_NET_SEC_OPEN   0u
#define DKM_NET_SEC_WEP    1u
#define DKM_NET_SEC_WPA    2u
#define DKM_NET_SEC_WPA2   3u
#define DKM_NET_SEC_WPA3   4u

/* ---- log API ---- */
typedef struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
} dkm_log_api;

/* ---- DMA API ---- */
typedef struct dkm_dma_buffer {
    void *virt;
    u64   phys;
    u64   size;
} dkm_dma_buffer;

typedef struct dkm_dma_api {
    int (*alloc_pages)(u64 page_count, u64 alignment, u64 max_phys, dkm_dma_buffer *out);
} dkm_dma_api;

/* ---- net API ---- */
typedef struct dkm_net_scan_result {
    char ssid[33];
    u8   bssid[6];
    u8   channel;
    i8   rssi;
    u8   security;
} dkm_net_scan_result;

typedef struct dkm_net_device_desc {
    const char *name;
    u8   mac[6];
    u32  flags;
    void *ctx;
    int (*tx)(void *ctx, const void *packet, u32 length);
    int (*rx_poll)(void *ctx, void *buffer, u32 capacity, u32 *out_length);
    /* 无线扩展回调（有线驱动置 NULL，向后兼容）。 */
    int (*scan_start)(void *ctx);
    int (*scan_count)(void *ctx);
    int (*scan_result)(void *ctx, u32 n, dkm_net_scan_result *out);
    int (*is_wireless)(void *ctx);
} dkm_net_device_desc;

typedef struct dkm_net_stats {
    u64 tx_ok;
    u64 tx_err;
    u64 rx_ok;
    u64 rx_err;
    u64 rx_overflow;
    u64 link_changes;
} dkm_net_stats;

typedef struct dkm_net_api {
    int  (*register_device)(const dkm_net_device_desc *desc);
    u32  (*device_count)(void);
    int  (*device_info)(u32 index, void *out);
    int  (*tx)(u32 index, const void *packet, u32 length);
    int  (*rx_poll)(u32 index, void *buffer, u32 capacity, u32 *out_length);
    int  (*scan_start)(u32 index);
    int  (*scan_count)(u32 index);
    int  (*scan_result)(u32 index, u32 n, dkm_net_scan_result *out);
    int  (*is_wireless)(u32 index);
    int  (*device_stats)(u32 index, dkm_net_stats *out);
} dkm_net_api;

/* ---- block API ---- */
typedef int (*dkm_block_read_fn)(void *ctx, u64 lba, u32 count, void *buffer);
typedef int (*dkm_block_write_fn)(void *ctx, u64 lba, u32 count, const void *buffer);

typedef struct dkm_block_device_desc {
    const char *name;
    u64 sector_size;
    u64 sector_count;
    void *ctx;
    int (*read)(void *ctx, u64 lba, u32 count, void *buffer);
    int (*write)(void *ctx, u64 lba, u32 count, const void *buffer);
} dkm_block_device_desc;

typedef struct dkm_block_api {
    int  (*register_device)(const dkm_block_device_desc *desc);
    u32  (*device_count)(void);
    int  (*read)(u32 index, u64 lba, u32 count, void *buffer);
    int  (*write)(u32 index, u64 lba, u32 count, const void *buffer);
    u64  (*sector_size)(u32 index);
    const char *(*device_name)(u32 index);
    int  (*set_write_fn)(u32 index, dkm_block_write_fn fn);
    u64  (*sector_count)(u32 index);  /* 0x38: tail-append, 与 utsm/block.h 同步 */
} dkm_block_api;

/* ---- MMIO / 页表 API ----
 * 与 UTSM/include/utsm/paging.h 布局一致。 */
typedef struct dkm_mmio_api {
    int  (*is_mapped)(u64 vaddr);   /* 返回 0=未映射；1=4K PTE；2=2M PDE；3=1G PDPTE */
    int  (*map_mmio)(u64 phys, u64 size);  /* 映射到 hhdm_offset + phys，返回 0 成功 */
    u64  (*cr3)(void);               /* 当前 CR3 物理地址（诊断用） */
} dkm_mmio_api;

/* ---- kernel_api 主结构 ----
 * 字段顺序/类型不可调整 - UTSM 端按固定偏移填充。
 * DSK 以 api+0xA8 硬偏移读取 block，net_stack 以 api+0x48 读取 net。 */
typedef struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;

    const dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const dkm_dma_api *dma;
    const void *vfs;
    const dkm_net_api *net;
    const void *timer;
    const void *drr;

    const void *rsdp_address;   /* ACPI RSDP physical address */
    const void *fb_address;     /* framebuffer base */
    u64 fb_width;
    u64 fb_height;
    u64 fb_pitch;
    u16 fb_bpp;
    const void *boot_modules_response;  /* limine_module_response* */
    int (*irq_register)(u8 irq, void *handler);  /* kernel irq_register */
    u64 hhdm_offset;                               /* physical -> virtual */
    const dkm_block_api *block;                    /* block provider registry */
    const dkm_mmio_api *mmio;                      /* 页表/高位 MMIO 映射服务 */
    /* -- 新增字段只允许追加在末尾 -- */
    void *(*mm_map_mmio)(u64 phys, u64 size);      /* MMIO 独立窗口映射（PCD|PWT），失败返回 0 */
    void (*mm_unmap_mmio)(void *virt, u64 size);   /* 解除 MMIO 独立窗口映射 */
    /* B7 阶段2: LAPIC EOI 钩子注册（apic.drv LAPIC 接管时调用）。 */
    void (*register_apic_eoi)(void (*eoi_fn)(void));
    /* B7 阶段3: 动态 IDT 向量分配器（MSI/MSI-X 用），池 0x40-0xDF。 */
    int  (*irq_vector_alloc)(void);
    void (*irq_vector_free)(int vector);
} dkm_kernel_api;

/* ---- kernel_api 字段偏移常量（消除散落的硬编码偏移） ----
 * 供不便通过结构体解引用的场景（如早期 boot context 解析）使用。
 * 常规代码应直接通过 api->net / api->block 访问。 */
#define DSB_KAPI_OFF_NET    0x48u   /* const dkm_net_api *net */
#define DSB_KAPI_OFF_BLOCK  0xA8u   /* const dkm_block_api *block */

#endif /* DESHAB_KERNEL_API_H */
