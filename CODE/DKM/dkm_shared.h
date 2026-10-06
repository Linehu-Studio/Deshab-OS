/* DKM 共享头文件 — 消除各驱动重复的结构体/宏/PCI/TSC 代码
 *
 * 设计要点:
 *   - 结构体定义与 UTSM/include/utsm/dkm.h 保持布局一致 (canonical)
 *   - PCI I/O 函数使用 static __inline__, 每个 .drv 文件获得独立副本, 避免链接冲突
 *   - TSC 校准函数使用 static __inline__, 每个 .drv 文件获得自己的 dkm_tsc_per_ms
 *   - 驱动源文件只需 #include "../dkm_shared.h", 保留各自的 driver_desc 和 driver_init
 *
 * 使用约定:
 *   1. 在 driver_init 开头调用 dkm_tsc_calibrate() 完成校准
 *   2. 时序等待使用 dkm_delay_ms() / dkm_delay_us()
 *   3. 串口发送等待使用 dkm_serial_wait_tx(COM1)
 *   4. MMIO 寄存器读取等待使用 dkm_rdtsc() + dkm_tsc_per_ms 直接计算 deadline
 */
#ifndef DKM_SHARED_H
#define DKM_SHARED_H

#include <stdint.h>

/* ---- 基本类型 (与 UTSM/types.h 一致) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef long long          i64;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- DKM magic / ABI 版本 ---- */
#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u

/* ---- DKM 驱动类别 (driver_desc.driver_class) ---- */
#define DKM_CLASS_PLATFORM  1u
#define DKM_CLASS_BUS       2u
#define DKM_CLASS_INTERRUPT 3u
#define DKM_CLASS_TIMER     4u
#define DKM_CLASS_CONSOLE   5u
#define DKM_CLASS_STORAGE   6u
#define DKM_CLASS_FS        7u
#define DKM_CLASS_NET       8u
#define DKM_CLASS_INPUT     9u
#define DKM_CLASS_GPU       10u
#define DKM_CLASS_MISC      11u

/* ---- DKM 驱动标志 (driver_desc.flags) ---- */
#define DKM_F_REQUIRED        (1u << 0)
#define DKM_F_BOOT_MODULE     (1u << 1)
#define DKM_F_NO_UNLOAD       (1u << 2)
#define DKM_F_STRONG_RECOVERY (1u << 3)
#define DKM_F_DMA_REQUIRED    (1u << 4)
#define DKM_F_EARLY_LOG       (1u << 5)

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

/* ---- DKM 结构体定义 (与 UTSM/include/utsm/dkm.h 布局一致) ---- */
struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
};

struct dkm_dma_buffer {
    void *virt;
    u64 phys;
    u64 size;
};

struct dkm_dma_api {
    int (*alloc_pages)(u64 page_count, u64 alignment, u64 max_phys, struct dkm_dma_buffer *out);
};

struct dkm_net_scan_result {
    char ssid[33];
    u8 bssid[6];
    u8 channel;
    i8 rssi;
    u8 security;
};

struct dkm_net_device_desc {
    const char *name;
    u8 mac[6];
    u32 flags;
    void *ctx;
    int (*tx)(void *ctx, const void *packet, u32 length);
    int (*rx_poll)(void *ctx, void *buffer, u32 capacity, u32 *out_length);
    /* 无线扩展回调（有线驱动置 NULL，向后兼容）。 */
    int (*scan_start)(void *ctx);
    int (*scan_count)(void *ctx);
    int (*scan_result)(void *ctx, u32 n, struct dkm_net_scan_result *out);
    int (*is_wireless)(void *ctx);
    /* 关联扩展（尾部追加，ABI 兼容；与 UTSM/include/utsm/net.h 布局一致）。 */
    int (*connect)(void *ctx, const char *ssid, const char *password);
    int (*disconnect)(void *ctx);
};

struct dkm_net_stats {
    u64 tx_ok;
    u64 tx_err;
    u64 rx_ok;
    u64 rx_err;
    u64 rx_overflow;
    u64 link_changes;
};

struct dkm_net_api {
    int (*register_device)(const struct dkm_net_device_desc *desc);
    u32 (*device_count)(void);
    int (*device_info)(u32 index, void *out);
    int (*tx)(u32 index, const void *packet, u32 length);
    int (*rx_poll)(u32 index, void *buffer, u32 capacity, u32 *out_length);
    int (*scan_start)(u32 index);
    int (*scan_count)(u32 index);
    int (*scan_result)(u32 index, u32 n, struct dkm_net_scan_result *out);
    int (*is_wireless)(u32 index);
    int (*connect)(u32 index, const char *ssid, const char *password);
    int (*disconnect)(u32 index);
    int (*device_stats)(u32 index, struct dkm_net_stats *out);
};

typedef int (*dkm_block_read_fn)(void *ctx, u64 lba, u32 count, void *buffer);
typedef int (*dkm_block_write_fn)(void *ctx, u64 lba, u32 count, const void *buffer);

struct dkm_block_device_desc {
    const char *name;
    u64 sector_size;
    u64 sector_count;
    void *ctx;
    int (*read)(void *ctx, u64 lba, u32 count, void *buffer);
    int (*write)(void *ctx, u64 lba, u32 count, const void *buffer);
};

struct dkm_block_api {
    int (*register_device)(const struct dkm_block_device_desc *desc);
    u32 (*device_count)(void);
    int (*read)(u32 index, u64 lba, u32 count, void *buffer);
    int (*write)(u32 index, u64 lba, u32 count, const void *buffer);
    u64 (*sector_size)(u32 index);
    const char *(*device_name)(u32 index);
    int (*set_write_fn)(u32 index, dkm_block_write_fn fn);
};

/* 与 UTSM/include/utsm/paging.h 中的 dkm_mmio_api 布局一致 */
struct dkm_mmio_api {
    /* 四级页表 walk。返回 0=未映射；1=4K PTE；2=2M PDE；3=1G PDPTE */
    int  (*is_mapped)(u64 vaddr);
    /* 把物理区间映射到 hhdm_offset + phys（4KiB UC 页），返回 0 成功 */
    int  (*map_mmio)(u64 phys, u64 size);
    /* 当前 CR3 物理地址（诊断用） */
    u64  (*cr3)(void);
};

/* 与 UTSM/include/utsm/dkm.h 中的 dkm_kernel_api 布局完全一致.
 * 字段顺序/类型不可调整 — UTSM 端按此偏移填充. */
struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const struct dkm_dma_api *dma;
    const void *vfs;
    const struct dkm_net_api *net;
    const void *timer;
    const void *drr;
    const void *rsdp_address;
    const void *fb_address;
    u64 fb_width;
    u64 fb_height;
    u64 fb_pitch;
    u16 fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(u8 irq, void *handler);
    u64 hhdm_offset;
    const struct dkm_block_api *block;   /* 不使用 block 的驱动可不访问 */
    const struct dkm_mmio_api *mmio;     /* 页表/高位 MMIO 映射服务（HHDM 空洞原地补映射） */
    /* —— 新增字段只允许追加在末尾（DSK 以 api+0xA8 硬偏移读取 block） —— */
    void *(*mm_map_mmio)(u64 phys, u64 size);    /* MMIO 独立窗口映射（PCD|PWT），返回虚拟地址，失败返回 0 */
    void (*mm_unmap_mmio)(void *virt, u64 size); /* 解除 MMIO 独立窗口映射 */
    /* B7 阶段2 尾部追加（旧内核无此字段 → 判空使用） */
    void (*register_apic_eoi)(void (*eoi_fn)(void));
    /* B7 阶段3 尾部追加：动态 IDT 向量分配器（MSI/MSI-X），池 0x40-0xDF */
    int  (*irq_vector_alloc)(void);
    void (*irq_vector_free)(int vector);
};

struct dkm_driver_handle;

struct dkm_driver_desc {
    u32 magic;
    u16 abi_version;
    u16 desc_size;
    const char *name;
    const char *version;
    const char *vendor;
    u32 driver_class;
    u32 stage;
    u32 flags;
    u32 priority;
    const char *const *depends;
    u32 depends_count;
    const char *const *provides;
    u32 provides_count;
    u64 min_kernel_abi;
    u64 feature_bits;
    u64 reserved0;
    u64 reserved1;
};

/* ---- PCI 配置空间常量 ---- */
#define PCI_ADDR       0xCF8
#define PCI_DATA       0xCFC
#define PCI_VENDOR_ID  0x00
#define PCI_DEVICE_ID  0x02
#define PCI_COMMAND    0x04
#define PCI_PROG_IF    0x09
#define PCI_SUBCLASS   0x0A
#define PCI_CLASS      0x0B
#define PCI_HEADER     0x0E
#define PCI_CAP_PTR    0x34
#define PCI_IRQ_LINE   0x3C
#define PCI_BAR0       0x10
#define PCI_BAR1       0x14
#define PCI_BAR2       0x18
#define PCI_BAR3       0x1C
#define PCI_BAR4       0x20
#define PCI_BAR5       0x24

#define PCI_CMD_IO     (1u << 0)
#define PCI_CMD_MEM    (1u << 1)
#define PCI_CMD_BUSM   (1u << 2)

/* ---- PCI I/O 端口内联函数 (每个 .drv 获得独立副本) ---- */
static __inline__ void dkm_outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u32 dkm_inl(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static __inline__ void dkm_outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u8 dkm_inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static __inline__ u32 dkm_pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 addr = (1u << 31)
             | ((u32)bus << 16)
             | ((u32)dev << 11)
             | ((u32)func << 8)
             | ((u32)reg & 0xFC);
    dkm_outl(PCI_ADDR, addr);
    return dkm_inl(PCI_DATA);
}

static __inline__ void dkm_pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 value) {
    u32 addr = (1u << 31)
             | ((u32)bus << 16)
             | ((u32)dev << 11)
             | ((u32)func << 8)
             | ((u32)reg & 0xFC);
    dkm_outl(PCI_ADDR, addr);
    dkm_outl(PCI_DATA, value);
}

static __inline__ u8 dkm_pci_read8(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 v = dkm_pci_read(bus, dev, func, reg);
    return (u8)((v >> ((reg & 3) * 8)) & 0xff);
}

/* ---- TSC 时序: 实机要求用 CPU 频率计算, 不用循环 ----
 * 每个 .drv 包含此头文件后获得自己的 file-local dkm_tsc_per_ms 副本,
 * 通过 dkm_tsc_calibrate() 在 driver_init 开头初始化.
 */
static u64 dkm_tsc_per_ms __attribute__((unused)) = 0;

static __inline__ u64 dkm_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 通过 PIT ch0 + TSC 校准 ~10ms 区间, 推算每毫秒 TSC 周期数. */
static __inline__ void dkm_tsc_calibrate(void) {
    dkm_outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    dkm_outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    dkm_outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = dkm_rdtsc();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        dkm_outb(0x43, 0x00);
        u16 cur = (u16)dkm_inb(0x40) | ((u16)dkm_inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = dkm_rdtsc();
    dkm_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static __inline__ void dkm_delay_ms(u32 ms) {
    if (!dkm_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000 * ms; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = dkm_tsc_per_ms * ms;
    u64 start = dkm_rdtsc();
    while (dkm_rdtsc() - start < target) __asm__ volatile("pause");
}

static __inline__ void dkm_delay_us(u32 us) {
    if (!dkm_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100 * us; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = dkm_tsc_per_ms * us / 1000;
    u64 start = dkm_rdtsc();
    while (dkm_rdtsc() - start < target) __asm__ volatile("pause");
}

/* 实机: 基于 TSC 的串口发送等待 (100us 超时). com_port = 0x3F8 (COM1). */
static __inline__ void dkm_serial_wait_tx(u16 com_port) {
    if (!dkm_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000; i++) {
            if (dkm_inb((u16)(com_port + 5)) & 0x20) break;
            __asm__ volatile("pause");
        }
        return;
    }
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms / 10;
    while (dkm_rdtsc() < deadline) {
        if (dkm_inb((u16)(com_port + 5)) & 0x20) break;
        __asm__ volatile("pause");
    }
}

#endif /* DKM_SHARED_H */
