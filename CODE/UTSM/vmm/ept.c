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

/* Leaf EPT entry: HPA + R/W/X + EPT memory type + Ignore PAT.
 * Nested KVM combines guest PAT with EPT type unless bit 6 is set;
 * WB+WC is reserved and VM-exits as EPT misconfiguration. */
static u64 ept_make_leaf(u64 hpa, u64 flags, u64 memtype) {
    return (hpa & 0x000FFFFFFFFFF000ULL) | (flags & 7ULL) |
           ((memtype & 7ULL) << 3) | EPT_IGNORE_PAT;
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
            /* 非叶 EPT 表项只含物理地址与 RWX。bits 5:3 在非叶项中
             * 保留，写入 leaf memory type 会触发 EPT misconfiguration。 */
            table[indices[level]] = new_phys | EPT_RWX;
            /* 切换到新分配的下一级页表 */
            table = (u64 *)phys_to_virt(new_phys);
        } else if ((entry & EPT_LARGE_PAGE) && level == 1) {
            /* PD 2MB leaf: split into 512×4K before installing a 4K PTE.
             * Treating the 2MB HPA as a page-table pointer would write PTEs
             * into guest RAM and leave the PD entry as a large page. */
            if (!alloc_missing) return (u64 *)0;
            u64 new_pt = ept_alloc_page();
            if (new_pt == 0) return (u64 *)0;
            u64 *pt = (u64 *)phys_to_virt(new_pt);
            u64 hpa_2m = entry & 0x000FFFFFFFE00000ULL;
            u64 page_flags = entry & 7ULL;
            for (int i = 0; i < 512; i++) {
                pt[i] = ept_make_leaf(hpa_2m + (u64)i * EPT_PAGE_SIZE,
                                      page_flags, EPT_MEMORY_TYPE_WB);
            }
            table[indices[level]] = new_pt | EPT_RWX;
            table = pt;
        } else {
            /* Strip reserved memory-type / IPAT / PS bits from non-leaf
             * table pointers. Those bits are only legal on page leaves. */
            u64 cleaned = entry & ~0xF8ULL;
            if (cleaned != entry)
                table[indices[level]] = cleaned;
            table = (u64 *)phys_to_virt(cleaned & 0x000FFFFFFFFFF000ULL);
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
        *entry = ept_make_leaf(cur_hpa, flags, EPT_MEMORY_TYPE_WB);
    }
    return 0;
}

int ept_identity_map(u64 gpa, u64 size, u64 flags) {
    return ept_map_range(gpa, gpa, size, flags);
}

/* ===== EPT 大页映射（2MB 大页，真机 MTRR 感知） =====
 *
 * 在 PD 层级直接映射 2MB 大页（bit 7 = EPT_LARGE_PAGE），
 * 减少 EPT walk 深度（4 级 → 3 级）和 TLB 压力。
 * 真机要求大页 memory type 与 MTRR 一致：
 *   - RAM 区域用 WB (6)
 *   - MMIO 区域用 UC (0)
 * QEMU 忽略 MTRR，但真机会检查。
 *
 * 用法：ept_map_2m_page(GPA, HPA, flags, memtype)
 *   flags = EPT_RWX 等
 *   memtype = EPT_MEMORY_TYPE_WB / EPT_MEMORY_TYPE_UC 等 */
int ept_map_2m_page(u64 gpa, u64 hpa, u64 flags, u64 memtype) {
    if (!g_ept_ready) return -1;
    /* GPA 和 HPA 必须 2MB 对齐 */
    if ((gpa & 0x1FFFFFULL) || (hpa & 0x1FFFFFULL)) {
        log_error("[EPT] 2MB page unaligned");
        return -2;
    }

    u64 *table = (u64 *)g_ept_pml4_virt;
    u64 pml4_idx = (gpa >> 39) & 0x1FF;
    u64 pdpt_idx = (gpa >> 30) & 0x1FF;
    u64 pd_idx   = (gpa >> 21) & 0x1FF;

    /* PML4 */
    u64 entry = table[pml4_idx];
    if (!(entry & EPT_READ)) {
        u64 new_phys = ept_alloc_page();
        if (new_phys == 0) return -3;
        table[pml4_idx] = new_phys | EPT_RWX;
        table = (u64 *)phys_to_virt(new_phys);
    } else {
        table = (u64 *)phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);
    }

    /* PDPT */
    entry = table[pdpt_idx];
    if (!(entry & EPT_READ)) {
        u64 new_phys = ept_alloc_page();
        if (new_phys == 0) return -4;
        table[pdpt_idx] = new_phys | EPT_RWX;
        table = (u64 *)phys_to_virt(new_phys);
    } else {
        table = (u64 *)phys_to_virt(entry & 0x000FFFFFFFFFF000ULL);
    }

    /* PD：直接写 2MB 大页项（Ignore PAT so nested KVM does not combine PAT） */
    u64 pd_entry = (hpa & 0x000FFFFFFFE00000ULL)  /* 物理地址（2MB 对齐） */
                 | flags                             /* R/W/X 权限 */
                 | EPT_LARGE_PAGE                    /* bit 7 = 大页标志 */
                 | ((memtype & 7ULL) << 3)           /* memory type */
                 | EPT_IGNORE_PAT;
    table[pd_idx] = pd_entry;
    return 0;
}

/* ===== INVVPID 支持（真机 VPID 刷新） =====
 *
 * 启用 VPID 后，真机要求在修改 EPT 映射后执行 INVVPID 刷新
 * stale TLB entry。QEMU 不检查，但真机会在以下场景触发 VMX abort：
 *   - guest 修改了 CR3 但 TLB 中缓存了旧 VPID 的映射
 *   - EPT 重映射后未刷新
 *   - VMCS VPID 字段变更后未执行 INVVPID
 *
 * INVVPID 类型：
 *   0 = 不支持
 *   1 = 单个 VPID 刷新（INVVPID_DESC.vpid）
 *   2 = 全局 VPID 刷新（所有 VPID）
 *   3 = 单个 VPID + PCID 刷新
 *
 * 当前实现：使用类型 2（全局刷新），最安全但性能最差。
 * 优化路径：在 EPT 重映射时调用类型 1 刷新特定 VPID。 */
static int g_invvpid_supported;
static int g_invept_supported;

void ept_check_vpid_support(void) {
    u64 cap = vmx_read_msr(IA32_VMX_EPT_VPID_CAP);
    /* INVVPID 类型 2 (all-context) 支持：bit 32 */
    g_invvpid_supported = (cap & (1ULL << 32)) ? 1 : 0;
    /* INVEPT 类型 2 (all-context) 支持：bit 25 */
    g_invept_supported = (cap & (1ULL << 25)) ? 1 : 0;
    log_hex64("[EPT] VPID cap=", cap);
    log_info(g_invvpid_supported ? "[EPT] INVVPID supported" : "[EPT] INVVPID NOT supported");
    log_info(g_invept_supported ? "[EPT] INVEPT supported" : "[EPT] INVEPT NOT supported");
}

/* 执行 INVEPT（EPT TLB 刷新）。真机要求在修改 EPT 后调用。 */
void ept_flush_ept(void) {
    if (!g_invept_supported) return;
    /* INVEPT 描述符必须 16 字节对齐；未对齐时指令失败，nested KVM
     * 会一直用缓存里的坏 EPT 项，表现为同一 GPA 的 misconfig 死循环。 */
    _Alignas(16) u64 desc[2];
    int err;
    desc[0] = ept_get_eptp();
    desc[1] = 0;
    __asm__ volatile(
        "invept (%2), %1\n"
        "ja 1f\n"
        "mov $1, %0\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "2:\n"
        : "=r"(err)
        : "r"((u64)1), "r"(desc)
        : "memory"
    );
    if (err) {
        desc[0] = 0;
        desc[1] = 0;
        __asm__ volatile(
            "invept (%2), %1\n"
            "ja 1f\n"
            "mov $1, %0\n"
            "jmp 2f\n"
            "1:\n"
            "mov $0, %0\n"
            "2:\n"
            : "=r"(err)
            : "r"((u64)2), "r"(desc)
            : "memory"
        );
    }
    if (err) {
        log_warn("[EPT] INVEPT failed (non-critical on QEMU)");
    }
}

/* 执行 INVVPID（VPID TLB 刷新）。真机要求在 VPID 变更后调用。 */
void ept_flush_vpid(u16 vpid) {
    if (!g_invvpid_supported) return;
    /* INVVPID 类型 2: all-context invalidate */
    u64 desc[2] = {0, 0};
    desc[0] = (u64)vpid;
    int err;
    __asm__ volatile(
        "invvpid (%2), %1\n"
        "ja 1f\n"
        "mov $1, %0\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "2:\n"
        : "=r"(err)
        : "r"((u64)2), "r"(desc)
        : "memory"
    );
    if (err) {
        log_warn("[EPT] INVVPID failed (non-critical on QEMU)");
    }
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
    if (hpa == 0 || hpa >= 0x100000000ULL) return 0;
    return hpa + offset;
}

int ept_repair_leaf(u64 gpa, u64 flags) {
    if (!g_ept_ready) return -1;

    u64 *table = (u64 *)g_ept_pml4_virt;
    u64 pml4_idx = (gpa >> 39) & 0x1FF;
    u64 pdpt_idx = (gpa >> 30) & 0x1FF;
    u64 pd_idx   = (gpa >> 21) & 0x1FF;
    u64 pt_idx   = (gpa >> 12) & 0x1FF;

    u64 entry = table[pml4_idx];
    if (!(entry & EPT_READ)) return -1;
    {
        u64 cleaned = entry & ~0xF8ULL;
        if (cleaned != entry)
            table[pml4_idx] = cleaned;
        table = (u64 *)phys_to_virt(cleaned & 0x000FFFFFFFFFF000ULL);
    }

    entry = table[pdpt_idx];
    if (!(entry & EPT_READ)) return -1;
    if (entry & EPT_LARGE_PAGE) {
        u64 hpa = entry & 0x000FFFFFC0000000ULL;
        table[pdpt_idx] = hpa | (flags & 7ULL) | EPT_LARGE_PAGE |
                          (EPT_MEMORY_TYPE_WB << 3) | EPT_IGNORE_PAT;
        return 0;
    }
    {
        u64 cleaned = entry & ~0xF8ULL;
        if (cleaned != entry)
            table[pdpt_idx] = cleaned;
        table = (u64 *)phys_to_virt(cleaned & 0x000FFFFFFFFFF000ULL);
    }

    entry = table[pd_idx];
    if (!(entry & EPT_READ)) return -1;
    if (entry & EPT_LARGE_PAGE) {
        u64 hpa = entry & 0x000FFFFFFFE00000ULL;
        table[pd_idx] = hpa | (flags & 7ULL) | EPT_LARGE_PAGE |
                        (EPT_MEMORY_TYPE_WB << 3) | EPT_IGNORE_PAT;
        return 0;
    }
    {
        u64 cleaned = entry & ~0xF8ULL;
        if (cleaned != entry)
            table[pd_idx] = cleaned;
        table = (u64 *)phys_to_virt(cleaned & 0x000FFFFFFFFFF000ULL);
    }

    entry = table[pt_idx];
    if (!(entry & 7ULL)) return -1;
    {
        u64 hpa = entry & 0x000FFFFFFFFFF000ULL;
        /* DMA pool is low 4G. A 4K leaf with PS, execute-only, or an
         * out-of-range HPA is leftover RAM interpreted as a PTE — keeping
         * that HPA (0xa5000008000 on the last boot) loops EPT misconfig. */
        if ((entry & EPT_LARGE_PAGE) || hpa == 0 || hpa >= 0x100000000ULL ||
            (entry & 7ULL) == EPT_EXECUTE)
            return -1;
        table[pt_idx] = ept_make_leaf(hpa, flags, EPT_MEMORY_TYPE_WB);
    }
    return 0;
}

void ept_log_walk(u64 gpa) {
    if (!g_ept_ready) return;
    u64 *table = (u64 *)g_ept_pml4_virt;
    u64 pml4_idx = (gpa >> 39) & 0x1FF;
    u64 pdpt_idx = (gpa >> 30) & 0x1FF;
    u64 pd_idx   = (gpa >> 21) & 0x1FF;
    u64 pt_idx   = (gpa >> 12) & 0x1FF;
    u64 e;
    log_hex64("[EPT] walk gpa=", gpa);
    e = table[pml4_idx];
    log_hex64("[EPT] pml4e=", e);
    if (!(e & EPT_READ)) return;
    table = (u64 *)phys_to_virt(e & 0x000FFFFFFFFFF000ULL);
    e = table[pdpt_idx];
    log_hex64("[EPT] pdpte=", e);
    if (!(e & EPT_READ) || (e & EPT_LARGE_PAGE)) return;
    table = (u64 *)phys_to_virt(e & 0x000FFFFFFFFFF000ULL);
    e = table[pd_idx];
    log_hex64("[EPT] pde=", e);
    if (!(e & EPT_READ) || (e & EPT_LARGE_PAGE)) return;
    table = (u64 *)phys_to_virt(e & 0x000FFFFFFFFFF000ULL);
    e = table[pt_idx];
    log_hex64("[EPT] pte=", e);
}

u64 ept_get_eptp(void) {
    if (!g_ept_ready) return 0;
    /* EPTP: bits [2:0]=memory type WB(6), bits [5:3]=walk length-1 (3 => 4-level).
     * Nested KVM accepted WB (eptp=...1e) on the passing self-test; forcing UC
     * after a misread of EPT_VPID_CAP bit 8 regressed to error 7. */
    return g_ept_pml4_phys | EPTP_WALK_LEN_4 | EPTP_MEMTYPE_WB;
}
