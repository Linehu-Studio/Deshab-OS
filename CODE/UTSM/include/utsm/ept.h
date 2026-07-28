#ifndef UTSM_EPT_H
#define UTSM_EPT_H

#include <utsm/types.h>

/* EPT 页表结构：4 级页表（PML4 / PDPT / PD / PT），每级 512 项，每页 4KB。 */

#define EPT_PAGE_SIZE   4096ULL
#define EPT_ENTRIES     512ULL

/* EPTP 构造：bits [5:0]=memory type, bits [7:6]=walk length-1, bits [51:12]=PML4 HPA */
#define EPTP_WALK_LEN_4  (3ULL << 6)   /* 4 级页表 */
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

#endif
