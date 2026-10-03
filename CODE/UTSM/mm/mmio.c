#include <utsm/mm.h>
#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include "../arch/x86_64/limine.h"

#define MM_PAGE_SIZE 4096ULL

/* 页表项属性位 */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_RW        (1ULL << 1)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_PS        (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_NX        (1ULL << 63)

#define CR4_LA57      (1ULL << 12)
#define CR4_PKE       (1ULL << 22)

/* MMIO 专用虚拟窗口：运行时在高半部 PML4[256..510] 中扫描空闲槽位，
 * 避开 Limine HHDM 区与内核映像（PML4[511]，0xFFFFFFFF80000000）。
 * 单个 PML4 槽位对应 512GiB 虚拟空间，bump 分配不回收。 */
#define MM_WINDOW_SIZE   (1ULL << 39)

extern volatile struct limine_hhdm_request g_hhdm_request;

static u64 g_hhdm;
static u64 g_pml4_phys;
static u64 g_window_base;
static u64 g_window_end;
static u64 g_window_next;
static int g_mm_state;   /* 0=未初始化, 1=就绪, -1=不可用 */
static u32 g_max_phys_addr_bits;  /* CPUID.80000008H:EAX[7:0], 真机物理地址宽度 */

static inline u64 mm_read_cr3(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline u64 mm_read_cr4(void) {
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void mm_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline void mm_invlpg(u64 virt) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* 在高半部 PML4[256..510] 中找一个空闲槽位作为 MMIO 窗口。
 * 返回该槽位覆盖的 canonical 虚拟基址；无空闲槽返回 0。 */
static u64 mm_find_window(void) {
    u64 *pml4 = (u64 *)(g_hhdm + g_pml4_phys);
    for (u64 idx = 256; idx < 511; idx++) {
        if (!(pml4[idx] & PTE_PRESENT)) {
            /* bit39..47 为槽位号，bit48 以上符号扩展为全 1 */
            return 0xFFFF000000000000ULL | (idx << 39);
        }
    }
    return 0;
}

/* 首次调用时懒初始化：取 HHDM offset 与当前 CR3。
 * UTSM 复用 Limine 页表，PML4 经 HHDM 访问。
 *
 * 真机安全检查:
 *   1. LA57 (5-level paging) 拒绝 — 页表 walk 假定 4 级
 *   2. CPUID 最大物理地址宽度检测 — 确保映射的物理地址合法
 *   3. PML4 槽位扫描 — 确认空闲窗口可用
 */
static void mm_lazy_init(void) {
    g_mm_state = -1;
    g_max_phys_addr_bits = 0;
    if (!g_hhdm_request.response) {
        log_warn("[MM] missing HHDM response");
        panic_full("MM-E10 MISSING HHDM RESPONSE",
                   "mm_lazy_init: Limine HHDM response missing", 0);
        return;
    }
    g_hhdm = g_hhdm_request.response->offset;
    if (!g_hhdm) {
        log_warn("[MM] null HHDM offset");
        panic_full("MM-E11 NULL HHDM OFFSET",
                   "mm_lazy_init: HHDM offset is zero", 0);
        return;
    }

    /* ---- 真机安全: LA57 早期检测与拒绝 ---- *
     * 若固件/BIOS 启用了 5 级分页 (CR4.LA57=1), 本实现的 4 级页表 walk
     * 将产生错误的地址转换, 必须拒绝继续运行。
     * **QEMU 默认不会启用 LA57, 此路径只在真机触发。** */
    if (mm_read_cr4() & CR4_LA57) {
        log_error("[MM] LA57 5-level paging ACTIVE — REFUSING to proceed");
        log_error("[MM] This kernel requires 4-level paging (48-bit virtual)");
        log_error("[MM] Disable LA57 in BIOS/firmware settings");
        panic_full("MM-E12 LA57 ACTIVE REFUSE",
                   "mm_lazy_init: 5-level paging active; 4-level walk required", 0);
        return;
    }

    /* ---- 真机安全: 物理地址宽度检测 ---- *
     * CPUID leaf 0x80000008 EAX[7:0] 给出 MaxPhysAddr 位宽。
     * 典型值: QEMU=40/46, 实机=46/48/52。
     * 若物理地址 > 48-bit, 4 级分页的 PTE 地址位可能不够用。
     * 当前 PTE_ADDR_MASK 只使用 bit12-51 (40 位物理页号 = 52 位物理地址),
     * 支持到 52-bit 物理地址, 足以覆盖所有现有 x86_64 实现。 */
    {
        u32 a, b, c, d;
        mm_cpuid(0x80000000, &a, &b, &c, &d);
        if (a >= 0x80000008) {
            mm_cpuid(0x80000008, &a, &b, &c, &d);
            g_max_phys_addr_bits = a & 0xff;
            log_hex64("[MM] CPU MaxPhysAddr bits=", g_max_phys_addr_bits);
            if (g_max_phys_addr_bits > 52) {
                log_error("[MM] physical address width > 52 bits — PTE cannot represent");
                panic_full("MM-E13 PHYS ADDR WIDTH EXCEEDED",
                           "mm_lazy_init: physical address width exceeds 52 bits", 0);
                return;
            }
        } else {
            /* 无 80000008H leaf, 假定 36 或 39 位 (旧 CPU) */
            g_max_phys_addr_bits = 36;
            log_warn("[MM] CPUID 80000008H not available; assuming 36-bit phys");
        }
    }

    g_pml4_phys = mm_read_cr3() & PTE_ADDR_MASK;
    g_window_base = mm_find_window();
    if (!g_window_base) {
        log_error("[MM] no free PML4 slot for mmio window");
        panic_full("MM-E14 NO FREE PML4 SLOT",
                   "mm_lazy_init: no free PML4 slot for mmio window", 0);
        return;
    }
    g_window_end = g_window_base + MM_WINDOW_SIZE;
    g_window_next = g_window_base;
    g_mm_state = 1;
    log_info("[MM] mmio mapper ready");
    log_hex64("[MM] PML4 phys=", g_pml4_phys);
    log_hex64("[MM] mmio window=", g_window_base);
    log_hex64("[MM] MaxPhysAddr bits=", g_max_phys_addr_bits);
}

/* 逐级 walk 到 PT，缺层时从 DMA 页分配器取清零页补层。
 * 返回最终 PTE 槽位的 HHDM 虚拟地址；冲突（撞上既有大页）或 OOM 返回 0。 */
static u64 *mm_walk_create(u64 virt) {
    u64 *table = (u64 *)(g_hhdm + g_pml4_phys);
    /* 中间三级：lvl=3 PML4(shift39) → lvl=2 PDPT(shift30) → lvl=1 PD(shift21) */
    for (int lvl = 3; lvl >= 1; lvl--) {
        u64 shift = 12 + (u64)lvl * 9;
        u64 idx = (virt >> shift) & 0x1FFULL;
        u64 entry = table[idx];
        if (entry & PTE_PRESENT) {
            if (entry & PTE_PS) return 0;   /* 既有 1GiB/2MiB 大页占用，冲突 */
            table = (u64 *)(g_hhdm + (entry & PTE_ADDR_MASK));
        } else {
            dkm_dma_buffer buf;
            if (dma_alloc_pages(1, MM_PAGE_SIZE, 0, &buf) != 0) {
                log_warn("[MM] page table alloc failed");
                panic_full("MM-E15 PAGE TABLE ALLOC FAILED",
                           "mm_walk_create: page table page alloc failed", 0);
                return 0;
            }
            table[idx] = buf.phys | PTE_PRESENT | PTE_RW;
            table = (u64 *)buf.virt;   /* dma 分配已清零 */
        }
    }
    return &table[(virt >> 12) & 0x1FFULL];
}

/* 只查询不创建的 walk；任一级缺失或为大页时返回 0。 */
static u64 *mm_walk_query(u64 virt) {
    u64 *table = (u64 *)(g_hhdm + g_pml4_phys);
    for (int lvl = 3; lvl >= 1; lvl--) {
        u64 shift = 12 + (u64)lvl * 9;
        u64 entry = table[(virt >> shift) & 0x1FFULL];
        if (!(entry & PTE_PRESENT) || (entry & PTE_PS)) return 0;
        table = (u64 *)(g_hhdm + (entry & PTE_ADDR_MASK));
    }
    return &table[(virt >> 12) & 0x1FFULL];
}

void *mm_map_mmio(u64 phys, u64 size) {
    if (g_mm_state == 0) mm_lazy_init();
    if (g_mm_state != 1 || !size) return 0;

    /* 真机安全: 验证物理地址在 CPU 支持的范围内 */
    if (g_max_phys_addr_bits > 0) {
        u64 max_phys = (1ULL << g_max_phys_addr_bits) - 1;
        if (phys > max_phys) {
            log_error("[MM] phys address exceeds CPU MaxPhysAddr");
            log_hex64("[MM] phys=", phys);
            log_hex64("[MM] max_phys=", max_phys);
            panic_full("MM-E16 PHYS EXCEEDS MAXADDR",
                       "mm_map_mmio: physical address exceeds CPU MaxPhysAddr", 0);
            return 0;
        }
    }

    u64 off = phys & (MM_PAGE_SIZE - 1ULL);
    u64 base_phys = phys - off;
    u64 total = (off + size + MM_PAGE_SIZE - 1ULL) & ~(MM_PAGE_SIZE - 1ULL);
    u64 pages = total / MM_PAGE_SIZE;

    u64 vbase = g_window_next;
    if (vbase < g_window_base || vbase + total > g_window_end) {
        log_error("[MM] mmio window exhausted");
        panic_full("MM-E17 MMIO WINDOW EXHAUSTED",
                   "mm_map_mmio: MMIO virtual window exhausted", 0);
        return 0;
    }

    u64 mapped = 0;
    for (u64 i = 0; i < pages; i++) {
        u64 virt = vbase + i * MM_PAGE_SIZE;
        u64 *pte = mm_walk_create(virt);
        if (!pte || (*pte & PTE_PRESENT)) {
            /* 冲突/失败：回滚本次已建立的映射 */
            log_error("[MM] map conflict");
            panic_full("MM-E18 MAP CONFLICT",
                       "mm_map_mmio: page table walk conflict or failure", 0);
            for (u64 j = 0; j < mapped; j++) {
                u64 v = vbase + j * MM_PAGE_SIZE;
                u64 *p = mm_walk_query(v);
                if (p) { *p = 0; mm_invlpg(v); }
            }
            return 0;
        }
        *pte = (base_phys + i * MM_PAGE_SIZE) | PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT;
        mm_invlpg(virt);
        mapped++;
    }

    g_window_next = vbase + total;
    log_hex64("[MM] map phys=", base_phys);
    log_hex64("[MM] map virt=", vbase);
    log_hex64("[MM] map pages=", pages);
    return (void *)(vbase + off);
}

void mm_unmap_mmio(void *virt, u64 size) {
    if (g_mm_state != 1 || !virt || !size) return;
    u64 v = (u64)virt & ~(MM_PAGE_SIZE - 1ULL);
    u64 end = ((u64)virt + size + MM_PAGE_SIZE - 1ULL) & ~(MM_PAGE_SIZE - 1ULL);
    for (; v < end; v += MM_PAGE_SIZE) {
        u64 *pte = mm_walk_query(v);
        if (pte && (*pte & PTE_PRESENT)) {
            *pte = 0;
            mm_invlpg(v);
        }
    }
    /* 窗口 bump 指针与中间页表页不回收：映射开销极小，保留复用 */
}
