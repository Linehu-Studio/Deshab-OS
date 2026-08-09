/* deshab/driver.h - DKM 驱动模块 ABI
 *
 * 整合 CODE/UTSM/include/utsm/dkm.h 与 CODE/DKM/dkm_shared.h 的驱动描述符定义。
 * 驱动模块（.drv，ET_REL）导出 driver_desc / driver_init / driver_exit 三个符号，
 * 由 DKM loader 校验 magic、调用 driver_init(api, handle) 完成初始化。
 *
 * 约定：
 *   - driver_init 返回 0 成功，负数失败。
 *   - required 驱动 init 失败触发 DRR recovery 或 panic。
 *   - driver_exit 仅用于非 NO_UNLOAD 驱动。
 *   - 驱动通过 dkm_kernel_api 调用内核服务，不直接依赖任意内核符号。
 */
#ifndef DESHAB_DRIVER_H
#define DESHAB_DRIVER_H

#include "types.h"
#include "kernel_api.h"

#define DKM_DRIVER_MAGIC 0x444B4D31u  /* "DKM1" */
#define DKM_ABI_VERSION  1u

/* ---- 驱动类别（driver_desc.driver_class） ---- */
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

/* ---- 驱动标志（driver_desc.flags） ---- */
#define DKM_F_REQUIRED        (1u << 0)  /* 当前 stage 失败则 DRR recovery 或 panic */
#define DKM_F_BOOT_MODULE     (1u << 1)  /* 可由 bootloader 预加载 */
#define DKM_F_NO_UNLOAD       (1u << 2)  /* 不允许卸载 */
#define DKM_F_STRONG_RECOVERY (1u << 3)  /* 驱动 data 段使用 UTSM 强恢复策略 */
#define DKM_F_DMA_REQUIRED    (1u << 4)  /* 驱动需要 DMA API */
#define DKM_F_EARLY_LOG       (1u << 5)  /* 驱动 init 期间允许使用 early log */

typedef struct dkm_driver_desc {
    u32 magic;              /* DKM_DRIVER_MAGIC */
    u16 abi_version;        /* DKM_ABI_VERSION */
    u16 desc_size;

    const char *name;
    const char *version;
    const char *vendor;

    u32 driver_class;       /* DKM_CLASS_* */
    u32 stage;              /* 加载阶段 0-3 */
    u32 flags;              /* DKM_F_* */
    u32 priority;

    const char *const *depends;
    u32 depends_count;

    const char *const *provides;
    u32 provides_count;

    u64 min_kernel_abi;
    u64 feature_bits;

    u64 reserved0;
    u64 reserved1;
} dkm_driver_desc;

struct dkm_driver_handle;

typedef int (*dkm_driver_init_fn)(const dkm_kernel_api *api, struct dkm_driver_handle *handle);
typedef int (*dkm_driver_exit_fn)(struct dkm_driver_handle *handle);

typedef enum dkm_driver_state {
    DKM_STATE_DISCOVERED = 0,
    DKM_STATE_ABI_CHECKED,
    DKM_STATE_INITING,
    DKM_STATE_ACTIVE,
    DKM_STATE_FAILED
} dkm_driver_state;

typedef struct dkm_driver_handle {
    const dkm_driver_desc *desc;
    dkm_driver_state state;
    i32 init_status;
    u32 load_stage;
    u32 flags;
} dkm_driver_handle;

typedef struct dkm_builtin_driver {
    const dkm_driver_desc *desc;
    dkm_driver_init_fn init;
    dkm_driver_exit_fn exit;
} dkm_builtin_driver;

#endif /* DESHAB_DRIVER_H */
