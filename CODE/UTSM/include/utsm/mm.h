#ifndef UTSM_MM_H
#define UTSM_MM_H

#include <utsm/types.h>

/* MMIO 映射接口：
 * mm_map_mmio 在当前页表（Limine 提供的 CR3）的 MMIO 专用窗口中
 * 建立 phys → virt 的 4K 页映射，页属性带 PCD|PWT（等效 UC），
 * 适用于任意物理地址（含 4G 以上的 PCI BAR）。
 * 返回的虚拟地址包含 phys 的页内偏移；失败返回 0。 */
void *mm_map_mmio(u64 phys, u64 size);

/* 解除 mm_map_mmio 建立的映射（按页清除 PTE + invlpg）。
 * 窗口虚拟地址与中间页表页不回收，保留复用。 */
void mm_unmap_mmio(void *virt, u64 size);

#endif
