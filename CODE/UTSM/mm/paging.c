/*
 * UTSM 页表/MMIO 映射服务实现
 *
 * 算法说明：
 *  - is_mapped: 标准 x86_64 四级页表 walk，O(1)（固定 4 级），
 *    页表页均为 RAM，经 HHDM 直映射访问，全程不触发 #PF。
 *  - map_mmio: 对 [phys, phys+size) 逐 4K 页在 HHDM 空间 (hhdm+phys)
 *    补建 P|RW|PCD|PWT 映射。若中间级已存在则复用；若整页已被
 *    Limine 大页覆盖则跳过（is_mapped 先行判定）。页表页由
 *    dma_alloc_pages 提供（低 4G、4K 对齐、返回即清零）。
 *  - not-present 项不会被 TLB 缓存，新映射无需 invlpg；
 *    仍对每页执行 invlpg 作为防御性措施（成本可忽略）。
 */

#include <utsm/paging.h>
#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

#define PTE_PRESENT   (1ULL << 0)
#define PTE_RW        (1ULL << 1)
#define PTE_PWT       (1ULL << 3)   /* MMIO: write-through */
#define PTE_PCD       (1ULL << 4)   /* MMIO: cache disable */
#define PTE_PS        (1ULL << 7)   /* 1G/2M 大页标志 */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PAGE_4K       4096ULL

static u64 paging_hhdm(void) {
    return g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
}

static u64 paging_read_cr3(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v & PTE_ADDR_MASK;
}

/* 返回 0=未映射；1=4K PTE；2=2M PDE；3=1G PDPTE */
static int paging_is_mapped_impl(u64 vaddr) {
    u64 hhdm = paging_hhdm();
    if (!hhdm) return 0;

    u64 *pml4 = (u64 *)(paging_read_cr3() + hhdm);
    u64 pml4e = pml4[(vaddr >> 39) & 0x1FF];
    if (!(pml4e & PTE_PRESENT)) return 0;

    u64 *pdpt = (u64 *)((pml4e & PTE_ADDR_MASK) + hhdm);
    u64 pdpte = pdpt[(vaddr >> 30) & 0x1FF];
    if (!(pdpte & PTE_PRESENT)) return 0;
    if (pdpte & PTE_PS) return 3;

    u64 *pd = (u64 *)((pdpte & PTE_ADDR_MASK) + hhdm);
    u64 pde = pd[(vaddr >> 21) & 0x1FF];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_PS) return 2;

    u64 *pt = (u64 *)((pde & PTE_ADDR_MASK) + hhdm);
    u64 pte = pt[(vaddr >> 12) & 0x1FF];
    if (!(pte & PTE_PRESENT)) return 0;
    return 1;
}

/* 分配一个清零的 4K 页作为页表页，返回物理地址；失败返回 0 */
static u64 paging_alloc_table(void) {
    dkm_dma_buffer buf;
    if (dma_alloc_pages(1, PAGE_4K, 0, &buf) != 0) return 0;
    return buf.phys;
}

static int paging_map_mmio_impl(u64 phys, u64 size) {
    u64 hhdm = paging_hhdm();
    if (!hhdm || !size) return -1;

    u64 begin = phys & ~(PAGE_4K - 1ULL);
    u64 end = (phys + size + PAGE_4K - 1ULL) & ~(PAGE_4K - 1ULL);
    u64 mapped = 0;
    u64 skipped = 0;

    for (u64 p = begin; p < end; p += PAGE_4K) {
        u64 v = hhdm + p;

        if (paging_is_mapped_impl(v)) {
            skipped++;          /* Limine 已覆盖（含 1G/2M 大页） */
            continue;
        }

        u64 *pml4 = (u64 *)(paging_read_cr3() + hhdm);
        u64 idx4 = (v >> 39) & 0x1FF;
        if (!(pml4[idx4] & PTE_PRESENT)) {
            u64 t = paging_alloc_table();
            if (!t) {
                log_error("[PAGING] alloc PDPT failed");
                panic_full("MM-E01 PDPT ALLOC FAILED",
                           "paging_map_mmio: PDPT page alloc failed", 0);
                return -2;
            }
            pml4[idx4] = t | PTE_PRESENT | PTE_RW;
        }

        u64 *pdpt = (u64 *)((pml4[idx4] & PTE_ADDR_MASK) + hhdm);
        u64 idx3 = (v >> 30) & 0x1FF;
        if (pdpt[idx3] & PTE_PRESENT) {
            if (pdpt[idx3] & PTE_PS) { skipped++; continue; }   /* 1G 大页已覆盖 */
        } else {
            u64 t = paging_alloc_table();
            if (!t) {
                log_error("[PAGING] alloc PD failed");
                panic_full("MM-E02 PD ALLOC FAILED",
                           "paging_map_mmio: PD page alloc failed", 0);
                return -3;
            }
            pdpt[idx3] = t | PTE_PRESENT | PTE_RW;
        }

        u64 *pd = (u64 *)((pdpt[idx3] & PTE_ADDR_MASK) + hhdm);
        u64 idx2 = (v >> 21) & 0x1FF;
        if (pd[idx2] & PTE_PRESENT) {
            if (pd[idx2] & PTE_PS) { skipped++; continue; }     /* 2M 大页已覆盖 */
        } else {
            u64 t = paging_alloc_table();
            if (!t) {
                log_error("[PAGING] alloc PT failed");
                panic_full("MM-E03 PT ALLOC FAILED",
                           "paging_map_mmio: PT page alloc failed", 0);
                return -4;
            }
            pd[idx2] = t | PTE_PRESENT | PTE_RW;
        }

        u64 *pt = (u64 *)((pd[idx2] & PTE_ADDR_MASK) + hhdm);
        u64 idx1 = (v >> 12) & 0x1FF;
        pt[idx1] = p | PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;
        __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        mapped++;
    }

    log_info("[PAGING] map_mmio done");
    log_hex64("[PAGING] phys=", phys);
    log_hex64("[PAGING] size=", size);
    log_hex64("[PAGING] 4K pages mapped=", mapped);
    log_hex64("[PAGING] pages already covered=", skipped);
    return 0;
}

static const dkm_mmio_api g_mmio_api = {
    .is_mapped = paging_is_mapped_impl,
    .map_mmio   = paging_map_mmio_impl,
    .cr3        = paging_read_cr3
};

const dkm_mmio_api *paging_get_api(void) {
    return &g_mmio_api;
}
