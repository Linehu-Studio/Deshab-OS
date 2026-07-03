#ifndef UTSM_PROCESS_H
#define UTSM_PROCESS_H

#include <utsm/types.h>
#include <utsm/uuid.h>
#include <utsm/capability.h>
#include <utsm/config.h>

/* ---- 进程标志 ---- */
#define UTSM_PROC_F_BOOTSTRAP  (1U << 0)   /* 内核 bootstrap 进程 */
#define UTSM_PROC_F_KERNEL     (1U << 1)   /* 内核态进程 */
#define UTSM_PROC_F_ENCRYPTED  (1U << 2)   /* 加密堆栈/堆 */

/* ---- 进程状态 ---- */
typedef enum {
    UTSM_PROC_UNUSED = 0,
    UTSM_PROC_ACTIVE,
    UTSM_PROC_BLOCKED,
    UTSM_PROC_TERMINATED
} utsm_process_state;

/* ---- Capability Table（每进程） ---- */
typedef struct {
    utsm_capability caps[UTSM_MAX_CAPABILITIES];
    u32 count;
    u32 generation;  /* 表版本号 */
} utsm_cap_table;

/* ---- Crypto Context（每进程） ---- */
typedef struct {
    u64 crypto_epoch;          /* 当前加密纪元 */
    u64 key_derivation_seed;   /* 密钥派生种子 */
    u32 pckc_hot_hint;         /* PCKC 热段提示 */
    u32 reserved;
} utsm_crypto_context;

/* ---- 进程上下文 ---- */
typedef struct utsm_process_context {
    uuid128_t process_uuid;
    u64 crypto_epoch;
    u64 recovery_generation;
    u32 process_slot;            /* 进程表索引 */
    u32 capability_count;
    u32 flags;
    utsm_process_state state;

    /* Capability Table */
    utsm_cap_table cap_table;

    /* Crypto Context */
    utsm_crypto_context crypto_ctx;

    /* PCKC 热段提示 */
    u32 hot_segment_slot;
    u32 reserved;

    /* DRR recovery generation */
    u64 drr_recovery_gen;
} utsm_process_context;

/* ---- 进程表 ---- */
#define UTSM_MAX_PROCESSES 64

/* ---- 公共接口 ---- */
void utsm_process_init(void);
void utsm_process_create_test(utsm_process_context *process, u64 seed);
utsm_process_context *utsm_process_alloc(void);
void utsm_process_free(utsm_process_context *process);
utsm_process_context *utsm_process_get(u32 slot);

/* Capability 操作 */
int utsm_process_grant_cap(utsm_process_context *process, utsm_capability cap);
int utsm_process_revoke_cap(utsm_process_context *process, u32 cap_index);
utsm_capability *utsm_process_get_cap(utsm_process_context *process, u32 cap_index);

/* Crypto context 操作 */
void utsm_process_advance_epoch(utsm_process_context *process);

#endif /* UTSM_PROCESS_H */
