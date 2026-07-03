#ifndef UTSM_DRR_H
#define UTSM_DRR_H

#include <utsm/types.h>
#include <utsm/config.h>

/* ===================================================================
 *  DRR — Dedicated Recovery Root
 *
 *  专用恢复根，独立于普通调度器和堆分配器。
 *  负责：root key、recovery log、checkpoint A/B、Emergency Pool。
 *
 *  约束：
 *    - DRR recovery 路径禁止依赖普通堆分配器
 *    - 普通 OOM 不影响 DRR recovery
 *    - Emergency Pool 耗尽时进入 system rollback
 * =================================================================== */

/* ---- Checkpoint 类型 ---- */
typedef enum {
    DRR_CKPT_LIGHT = 0,        /* 控制面快照 */
    DRR_CKPT_DIRTY_PAGE,       /* dirty ciphertext page 快照 */
    DRR_CKPT_BOOT              /* 启动状态 + A/B slot */
} drr_checkpoint_type;

/* ---- Recovery Log 条目 ---- */
typedef struct {
    u32 segment_slot;
    u32 flags;          /* 0=intent, 1=commit */
    u64 offset;
    u64 length;
    u64 timestamp;      /* writer_seq snapshot */
} drr_log_entry;

#define DRR_LOG_FLAG_INTENT  0u
#define DRR_LOG_FLAG_COMMIT  1u

/* ---- Checkpoint Metadata Slot ---- */
typedef struct {
    u64 magic;          /* DRR_CKPT_MAGIC */
    u32 version;
    u32 active;         /* 0=inactive, 1=active */
    drr_checkpoint_type type;
    u64 global_epoch;
    u64 dirty_page_count;
    u64 mac_root;       /* MAC root hash of all sealed pages */
    u64 segment_count;
    u64 crc;            /* CRC64 of the above */
} drr_checkpoint_meta;

#define DRR_CKPT_MAGIC    0x445252434B505421ULL  /* "DRRCKPT!" */
#define DRR_CKPT_VERSION  1u

/* ---- Emergency Pool ---- */
#define DRR_EMERGENCY_POOL_SIZE  (64ULL * 1024ULL)  /* 64KB 独立池 */
#define DRR_EMERGENCY_STACK_SIZE 4096ULL
#define DRR_RECOVERY_LOG_SIZE    4096ULL
#define DRR_CKPT_META_BUF_SIZE  4096ULL
#define DRR_CRASH_BUF_SIZE      2048ULL
#define DRR_META_SLAB_SIZE      2048ULL
#define DRR_EMERGENCY_PAGES     4

typedef struct {
    u8  pool[DRR_EMERGENCY_POOL_SIZE] __attribute__((aligned(64)));
    u64 pool_offset;
    u64 pool_limit;

    /* 子缓冲区指针（pool 内偏移分配） */
    u8  *stack_base;
    u64  stack_size;
    u64  stack_ptr;

    drr_log_entry *log_base;
    u32  log_count;
    u32  log_capacity;

    drr_checkpoint_meta *ckpt_meta_buf;
    u8  *crash_buf;
    u64  crash_buf_size;
    void *meta_slab;
    u64  meta_slab_offset;

    /* A/B checkpoint slots */
    drr_checkpoint_meta ckpt_a;
    drr_checkpoint_meta ckpt_b;
    u32 active_slot;    /* 0=A, 1=B */

    /* Root key */
    const u64 *root_key;

    /* Recovery generation */
    u64 recovery_generation;

    /* Heartbeat */
    u64 heartbeat_counter;
    u64 heartbeat_interval;

    /* 初始化标记 */
    int initialized;
} drr_state;

/* ---- 公共接口 ---- */

void drr_init(void);
const u64 *drr_get_root_key(void);
u64 drr_recovery_generation(void);
void drr_report_fault(const char *reason);
void drr_log_write_intent(u32 segment_slot, u64 offset, u64 len);
void drr_log_write_commit(u32 segment_slot, u64 offset, u64 len);

/* Checkpoint 操作 */
int drr_checkpoint_begin(drr_checkpoint_type type);
int drr_checkpoint_finish(void);
int drr_checkpoint_recover(drr_checkpoint_meta *out_meta);

/* Emergency Pool 分配 */
void *drr_emergency_alloc(u64 size, u64 alignment);
void drr_emergency_reset(void);

/* Heartbeat */
void drr_heartbeat_tick(void);
u64 drr_heartbeat_get(void);

/* CRC64 */
u64 drr_crc64(const void *data, u64 len);

#endif /* UTSM_DRR_H */
