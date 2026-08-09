#ifndef UTSM_XJ380_LOADER_H
#define UTSM_XJ380_LOADER_H

#include <utsm/types.h>

/* xj380_loader.h — OpenXJ380 (kernel.krl) guest loader for UTSM VMX non-root guest.
 *
 * 三内核架构路线 B：UTSM hypervisor 同时运行 Linux 与 OpenXJ380 两个 VM guest。
 * 本模块把 OpenXJ380 编译产物 kernel.krl（固定链接 0xFFFFFFFF80000000 的
 * ELF64 ET_EXEC）作为第二 guest 加载：
 *   - 解析 ELF PT_LOAD 段 → 装入 guest 物理内存
 *   - 构造 guest 初始页表（恒等 + HHDM + 内核区三区布局，同 XJ380 引导器契约）
 *   - 构造 BOOT_CONFIG / FrameBufferConfig / 伪 EFI_SYSTEM_TABLE 交接
 *   - 构造 MADT/FADT/HPET/MCFG 桩表（LAPIC/IOAPIC/HPET/MMCONF 均为幽灵地址）
 *   - 建立 EPT 映射（boot area / framebuffer / kernel / RAM）
 *
 * XJ380 内核侧启动契约（证据见 OpenXJ380/boot/include/boot.h 与
 * OpenXJ380/kernel/main.cpp / memory/frame.cpp）：
 *   - KernelMain(fbc, SystemTable, BootConfig) — 3 个 SysV 参数，引用退化为指针
 *   - BOOT_CONFIG 在入口立即被 memcpy 拷贝（main.cpp:281），原缓冲仅需入口可读
 *   - MemoryMap.Buffer / saved_mtrrs 是物理地址，内核自行 +0xffff800000000000
 *   - temp_stack[] 是 HHDM 虚拟地址数组（仅拷贝，1 CPU 时不 deref）
 *   - MADT 在清低半（main.cpp:392）之前按物理地址直读，之后经 HHDM 读
 *
 * 里程碑 M1：guest 启动至根文件系统挂载（预期在 mount_root 死循环处停止，
 * 无虚拟块设备）。串口经 UTSM COM1 模拟可见 XJ380 启动全程。
 */

/* ===== XJ380 guest GPA 布局 =====
 *
 *  0x00000000-0x0000FFFF : 保留（XJ380 AP 跳板区 APU_BASE_ADDR=0x10000）
 *  0x00020000-0x00033FFF : UTSM loader 私有 boot area（<1MB，内核不分配）
 *                          page tables / GDT / ACPI 桩表 / 内存图 /
 *                          BOOT_CONFIG / MTRR 缓冲 / stack / 伪 EFI 表
 *  0x00100000-0x00400000 : framebuffer 1024x768x4（3MB，隐式保留）
 *  0x01000000-0x02FFFFFF : kernel.krl 内核镜像窗口（32MB，隐式保留）
 *  0x03000000-0x43000000 : guest RAM 1GB（内存图中唯一的 Conventional 区）
 */
#define XJ380_KERNEL_VADDR     0xFFFFFFFF80000000ULL  /* XJ380 linker.ld 固定链接地址 */
#define XJ380_HHDM_OFFSET      0xFFFF800000000000ULL  /* XJ380 HHDM 常量（hhdm.cpp:15） */

#define XJ380_BOOT_AREA_GPA    0x00020000ULL          /* 128KB，避开 AP 跳板 0x10000 */
#define XJ380_PGT_GPA          (XJ380_BOOT_AREA_GPA + 0x0000)   /* 8 页: PML4/PDPT0/PD0-3/PDPT_K/PD_K */
#define XJ380_PGT_SIZE         (8 * 4096)
#define XJ380_GDT_GPA          (XJ380_PGT_GPA + XJ380_PGT_SIZE)
#define XJ380_MADT_GPA         (XJ380_GDT_GPA + 0x1000)
#define XJ380_FADT_GPA         (XJ380_MADT_GPA + 0x1000)
#define XJ380_MCFG_GPA         (XJ380_FADT_GPA + 0x1000)
#define XJ380_HPET_GPA         (XJ380_MCFG_GPA + 0x1000)
#define XJ380_MEMMAP_GPA       (XJ380_HPET_GPA + 0x1000)
#define XJ380_BOOTCFG_GPA      (XJ380_MEMMAP_GPA + 0x1000)
#define XJ380_MTRR_GPA         (XJ380_BOOTCFG_GPA + 0x1000)
#define XJ380_STACK_GPA        (XJ380_MTRR_GPA + 0x2000)   /* 8KB guest 启动栈 */
#define XJ380_FAKEEFI_GPA      (XJ380_STACK_GPA + 0x2000)
#define XJ380_FBC_GPA          (XJ380_FAKEEFI_GPA + 0x1000)
#define XJ380_BOOT_AREA_SIZE   (XJ380_FBC_GPA + 0x1000 - XJ380_BOOT_AREA_GPA) /* 至 FBC 结束 */
#define XJ380_BOOT_STACK_TOP   (XJ380_STACK_GPA + 0x2000)

#define XJ380_GUEST_FB_GPA     0x00100000ULL          /* 1MB */
#define XJ380_FB_WIDTH         1024
#define XJ380_FB_HEIGHT        768
#define XJ380_FB_BPP           4
#define XJ380_FB_SIZE          ((u64)XJ380_FB_WIDTH * XJ380_FB_HEIGHT * XJ380_FB_BPP)

#define XJ380_GUEST_KERNEL_GPA 0x01000000ULL          /* 16MB */
#define XJ380_GUEST_KERNEL_MAX (32 * 1024 * 1024)     /* 内核窗口 16MB..48MB */

#define XJ380_GUEST_RAM_GPA    0x03000000ULL          /* 48MB */
#define XJ380_GUEST_RAM_SIZE   0x40000000ULL          /* 1GB (0x03000000..0x43000000) */
#define XJ380_GUEST_RAM_MIN    0x10000000ULL          /* 降级下限 256MB */

/* BOOT_CONFIG.boot_flags（与 OpenXJ380 boot/include/boot.h 一致） */
#define XJ380_BOOT_FLAG_DISABLE_KMOD   (1ULL << 2)
#define XJ380_BOOT_FLAG_SAFE_STORAGE_IO (1ULL << 3)

/* ===== 加载后的 XJ380 guest 状态 ===== */
struct xj380_guest_info {
    u64 kernel_gpa;         /* 内核段装入的 GPA 基址 */
    u64 kernel_entry;       /* 64 位入口（guest 虚拟地址 0xFFFFFFFF80000000+） */
    u64 kernel_size;        /* 内核跨度（memsz 上界 - 基址） */
    u64 pgt_gpa;            /* guest CR3（页表 GPA） */
    u64 gdt_gpa;            /* guest GDT GPA */
    u64 ram_gpa;            /* guest RAM GPA 基址 */
    u64 ram_size;           /* guest RAM 实际大小 */
    u64 fb_gpa;             /* framebuffer GPA */
    int loaded;             /* 1 = 加载成功 */
};

/* 在 Limine boot module 中查找 XJ380 内核（路径含 "xj380"）。
 * 返回模块数据指针，*size_out 为大小；未找到返回 NULL。 */
void *xj380_find_kernel_module(u64 *size_out);

/* 初始化 XJ380 guest：解析 ELF、分配内存、构造页表/GDT/ACPI 桩表/
 * BOOT_CONFIG/内存图、EPT 映射。返回 0 成功，负值失败。 */
int xj380_loader_init(void);

/* 获取加载后的 guest 信息（未加载返回 NULL）。 */
const struct xj380_guest_info *xj380_get_guest_info(void);

/* 配置 VMCS 并 vmlaunch XJ380 guest。
 * 前置条件：vmm_init() + xj380_loader_init()。
 * guest 终止（异常/三重故障/0xCF9 重启）后返回；期间 host 挂起。
 * 返回 0 表示 guest 终止返回，负值表示启动失败。 */
int xj380_launch(void);

#endif /* UTSM_XJ380_LOADER_H */
