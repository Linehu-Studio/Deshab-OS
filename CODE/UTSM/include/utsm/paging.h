#ifndef UTSM_PAGING_H
#define UTSM_PAGING_H

#include <utsm/types.h>

/*
 * UTSM 页表/MMIO 映射服务
 *
 * 背景：内核使用 Limine 建立的页表 + HHDM 直映射访问全部 RAM，
 * 但 HHDM 只保证覆盖内存映射中的 RAM 区域，不保证覆盖 4G 以上的
 * PCI MMIO 空洞（QEMU/OVMF 常把 NVMe 等 64-bit BAR 分配到 48GiB
 * 附近）。本模块提供：
 *   1. is_mapped —— 四级页表只读 walk，运行时核实某虚拟地址是否可访问，
 *      用于在触碰高位 BAR 前先确认 HHDM 覆盖情况（零 #PF 风险）。
 *   2. map_mmio  —— 在 Limine 页表的 HHDM 空洞处原地补充 4KiB UC 映射
 *      （P|RW|PCD|PWT），页表页取自 DMA 物理页分配器（低 4G、4K 对齐、清零）。
 *
 * SAS-R0 单地址空间模型：不新建地址空间，直接扩展当前 CR3 指向的页表，
 * 所有 CPU/驱动立即可见，无切换成本。
 */

typedef struct dkm_mmio_api {
    /* 四级页表 walk。返回 0=未映射；1=4K PTE 命中；2=2M PDE 命中；3=1G PDPTE 命中 */
    int  (*is_mapped)(u64 vaddr);
    /* 把物理区间 [phys, phys+size) 映射到 hhdm_offset + phys，4KiB UC 页。
     * 已被 Limine 覆盖（含 1G/2M 大页）的页自动跳过。返回 0 成功，负数失败。 */
    int  (*map_mmio)(u64 phys, u64 size);
    /* 当前 CR3 物理地址（诊断用） */
    u64  (*cr3)(void);
} dkm_mmio_api;

const dkm_mmio_api *paging_get_api(void);

#endif
