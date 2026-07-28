#include <utsm/ept.h>
#include <utsm/vmx.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* EPT 4 级页表：PML4[512] -> PDPT[512] -> PD[512] -> PT[512] -> 4KB 页
 * 虚拟地址（GPA）拆分：
 *   bits [51:48] reserved
 *   bits [47:39] PML4 index
 *   bits [38:30] PDPT index
 *   bits [29:21] PD index
 *   bits [20:12] PT index
 *   bits [11:0]  page offset
 *
 * EPT entry 格式（4KB 页）：
 *   bits [2:0]   R/W/X
 *   bits [5:3]   memory type
 *   bit  6       ignore PAT
 *   bit  7       large page
 *   bits [11:8]  ignored
 *   bits [51:12] physical address (page-aligned)
 */

static u64 g_ept_pml4_phys;
static u64 g_ept_pml4_virt;
static int g_ept_ready;

/* 物理地址 → 虚拟地址（HHDM 直映射） */
static void *phys_to_virt(u64 phys) {
    if (!g_hhdm_request.response) return (void *)0;
    u64 hhdm = g_hhdm_request.response->offset;
    return (void *)(hhdm + phys);
}

/* 分配一页 4KB 对齐物理页，返回物理地址；置零。 */
static u64 ept_alloc_page(void) {
    dkm_dma_buffer buf;
    if (dma_alloc_pages(1, EPT_PAGE_SIZE, 0x100000000ULL, &buf) != 0) {
        log_error("[EPT] alloc page failed");
        return 0;
    }
    /* dma_alloc_pages 已清零 */
    return buf.phys;
}

int ept_init(void) {
    g_ept_pml4_phys = ept_alloc_page();
    if (g_ept_pml4_phys == 0) {
        log_error("[EPT] failed to alloc PML4");
        return -1;
    }
    g_ept_pml4_virt = (u64)phys_to_virt(g_ept_pml4_phys);
    g_ept_ready = 1;
    log_hex64("[EPT] PML4 phys=", g_ept_pml4_phys);
    log_info("[EPT] init ok");
    return 0;
}

/* 读取 EPT 表项指针，必要时分配中间页表。
 * level: 0=PT, 1=PD, 2=PDPT, 3=PML4 */
static u64 *ept_walk(u64 gpa, int alloc_missing) {
    u64 *table = (u64 *)g_ept_pml4_virt;
    u64 indices[4];
    indices[3] = (gpa >> 39) & 0x1FF;
    indices[2] = (gpa >> 30) & 0x1FF;
    indices[1] = (gpa >> 21) & 0x1FF;
    indices[0] = (gpa >> 12) & 0x1FF;

    for (int level = 3; level > 0; level--) {
        u64 entry = table[indices[level]];
        if (!(entry & EPT_READ)) {
            if (!alloc_missing) return (u64 *)0;
            u64 new_phys = ept_alloc_page();
            if (new_phys == 0) return (u64 *)0;
            /* 中间页表项：RWX + WB 内存类型 */
            table[indices[level]] = new_phys | EPT_RWX | (EPT_MEMORY_TYPE_WB << 3);
            /* 切换到新分配的下一级页表 */
            table = (u64 *)phys_to_virt(new_phys);
        } else {
            /* 清除标志位取出物理地址 */
            u64 child_phys = entry & 0x000FFFFFFFFFF000ULL;
            table = (u64 *)phys_to_virt(child_phys);
        }
    }
    return &table[indices[0]];
}

int ept_map_range(u64 gpa, u64 hpa, u64 size, u64 flags) {
    if (!g_ept_ready) return -1;
    if ((gpa & (EPT_PAGE_SIZE - 1)) || (hpa & (EPT_PAGE_SIZE - 1)) ||
        (size & (EPT_PAGE_SIZE - 1))) {
        log_error("[EPT] unaligned map request");
        return -2;
    }

    u64 pages = size / EPT_PAGE_SIZE;
    for (u64 i = 0; i < pages; i++) {
        u64 cur_gpa = gpa + i * EPT_PAGE_SIZE;
        u64 cur_hpa = hpa + i * EPT_PAGE_SIZE;
        u64 *entry = ept_walk(cur_gpa, 1);
        if (!entry) {
            log_error("[EPT] walk failed");
            return -3;
        }
        *entry = cur_hpa | flags | (EPT_MEMORY_TYPE_WB << 3);
    }
    return 0;
}

int ept_identity_map(u64 gpa, u64 size, u64 flags) {
    return ept_map_range(gpa, gpa, size, flags);
}

/* Walk the EPT to translate a GPA to its mapped HPA.
 * Returns 0 if the GPA is not mapped (no present entry at any level).
 * Handles 4KB pages and 2MB large pages (bit 7 = PS in PD entry). */
u64 ept_gpa_to_hpa(u64 gpa) {
    if (!g_ept_ready) return 0;

    u64 *table = (u64 *)g_ept_pml4_virt;
    u64 pml4_idx = (gpa >> 39) & 0x1FF;
    u64 pdpt_idx = (gpa >> 30) & 0x1FF;
    u64 pd_idx   = (gpa >> 21) & 0x1FF;
    u64 pt_idx   = (gpa >> 12) & 0x1FF;
    u64 offset   = gpa & 0xFFF;

    /* Level 3: PML4 */
    u64 entry = table[pml4_idx];
    if (!(entry & EPT_READ)) return 0;
    table = (u64 *)phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);

    /* Level 2: PDPT */
    entry = table[pdpt_idx];
    if (!(entry & EPT_READ)) return 0;
    /* 1GB large page (bit 7 set in PDPT entry) */
    if (entry & EPT_LARGE_PAGE) {
        u64 hpa = entry & 0x000FFFFFC0000000ULL;  /* 1GB page base */
        return hpa + (gpa & 0x3FFFFFFF);          /* offset within 1GB */
    }
    table = (u64 *)phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);

    /* Level 1: PD */
    entry = table[pd_idx];
    if (!(entry & EPT_READ)) return 0;
    /* 2MB large page (bit 7 set in PD entry) */
    if (entry & EPT_LARGE_PAGE) {
        u64 hpa = entry & 0x000FFFFFFFE00000ULL;  /* 2MB page base */
        return hpa + (gpa & 0x1FFFFF);            /* offset within 2MB */
    }
    table = (u64 *)phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);

    /* Level 0: PT */
    entry = table[pt_idx];
    if (!(entry & EPT_READ)) return 0;
    u64 hpa = entry & 0x000FFFFFFFFFF000ULL;
    return hpa + offset;
}

u64 ept_get_eptp(void) {
    if (!g_ept_ready) return 0;
    /* EPTP: bits [5:0] = memtype, bits [7:6] = walk length-1 (3 for 4-level),
     * bits [51:12] = PML4 HPA */
    return g_ept_pml4_phys | EPTP_WALK_LEN_4 | EPTP_MEMTYPE_WB;
}
