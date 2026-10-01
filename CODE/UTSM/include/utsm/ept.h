#ifndef UTSM_EPT_H
#define UTSM_EPT_H

#include <utsm/types.h>

/* EPT 页表结构：4 级页表（PML4 / PDPT / PD / PT），每级 512 项，每页 4KB。 */

#define EPT_PAGE_SIZE   4096ULL
#define EPT_ENTRIES     512ULL

/* EPTP 构造：bits [2:0]=memory type, bits [5:3]=walk length-1, bit 6=AD, bits [51:12]=PML4 HPA
 * P8.4 Fix: walk length was at bits [7:6] (wrong), should be bits [5:3] per Intel SDM. */
#define EPTP_WALK_LEN_4  (3ULL << 3)   /* 4 级页表 (3 = 4-1) at bits [5:3] */
#define EPTP_MEMTYPE_WB  (EPT_MEMORY_TYPE_WB)

/* 初始化 EPT：分配 PML4，置零。返回 0 成功。 */
int ept_init(void);

/* 构造 EPTP 值（用于 VMCS EPT_POINTER 字段）。 */
u64 ept_get_eptp(void);

/* 在 EPT 中映射一段 GPA→HPA 区间，权限由 flags 决定（EPT_READ/WRITE/EXECUTE）。
 * 自动按 4KB 分配中间页表页。返回 0 成功。 */
int ept_map_range(u64 gpa, u64 hpa, u64 size, u64 flags);

/* 便利函数：1:1 映射（GPA==HPA）。 */
int ept_identity_map(u64 gpa, u64 size, u64 flags);

/* Walk the EPT to translate a GPA to its mapped HPA.
 * Returns the HPA on success, 0 if the GPA is not mapped.
 * Handles 4KB pages and 2MB large pages. */
u64 ept_gpa_to_hpa(u64 gpa);

/* Rewrite an existing EPT leaf (4K or 2MB) as RWX+WB+Ignore PAT.
 * Returns 0 if a leaf was found and rewritten, -1 if unmapped.
 * Used to recover from nested-KVM EPT misconfig (guest PAT vs EPT type). */
int ept_repair_leaf(u64 gpa, u64 flags);

/* Log PML4/PDPT/PD/PT entries for a GPA (misconfig diagnostics). */
void ept_log_walk(u64 gpa);

/* Map a 2MB large page in EPT (PD-level direct mapping).
 * GPA and HPA must be 2MB-aligned. memtype = EPT_MEMORY_TYPE_*.
 * Required for real hardware MTRR compliance. */
int ept_map_2m_page(u64 gpa, u64 hpa, u64 flags, u64 memtype);

/* Check INVVPID/INVEPT support from IA32_VMX_EPT_VPID_CAP MSR.
 * Call once after VMX init. */
void ept_check_vpid_support(void);

/* Flush EPT TLB entries (INVEPT). Required after EPT mapping changes on real hardware. */
void ept_flush_ept(void);

/* Flush VPID TLB entries (INVVPID). Required after VPID changes on real hardware. */
void ept_flush_vpid(u16 vpid);

#endif
