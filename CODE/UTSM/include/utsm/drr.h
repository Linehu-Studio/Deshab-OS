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
/* F1: 崩溃归档（crash_buf 写入；返回 0=已归档，-1=DRR 未初始化） */
int drr_crash_archive(const char *symbol, const char *msg, const void *regs);
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

/* ===================================================================
 *  Phase 8 扩展：真实脏页快照 / 页级回滚 / 看门狗 / system rollback
 *  （尾部追加，不动既有结构与语义）
 * =================================================================== */

/* 快照区条目（快照区第 0 页：头(64B) + 条目数组顺序存放） */
typedef struct {
    u32 seg_slot;       /* utsm_get_segment 的 slot */
    u32 page_index;     /* 段内页号 */
    u64 cipher_vaddr;   /* 段内目标页虚拟地址（HHDM，回滚目标） */
    u64 crc;            /* 快照数据 CRC64（drr_crc64，区域完整性） */
    u32 region_idx;     /* 快照数据所在页序号（区域第 1+idx 页） */
    u32 used;
    /* Phase 9: 真实 MAC（keyed BLAKE2b，绑定 seg/page/epoch） */
    u64 key_epoch;      /* 快照时刻段 key_epoch（MAC 上下文 + 防跨纪元重放） */
    u64 mac;            /* 页 MAC（utsm_page_mac，截断 64 位） */
} drr_ckpt_page_entry;

/* 真实脏页快照：扫描全部 ACTIVE 段的 dirty shard 位图，逐脏页复制到
 * 快照区（先快照后清位；区满则部分提交且不清位），随后走 A/B checkpoint
 * 原子切换。O(dirty_shards + dirty_pages)。
 * 返回快照页数（>=0）；负数失败（未初始化/快照区不可用）。 */
int drr_ckpt_snapshot_dirty(void);

/* 页级回滚：按 active slot 元数据 + 快照区逐页 CRC 校验后恢复密文页
 * 并清除对应脏位；触碰段状态 RECOVERING → ACTIVE，CRC 失败段 POISONED。
 * 返回恢复页数（>=0）；失败 -2（任一页 CRC 不符）。 */
int drr_rollback_pages(void);

/* 系统级回滚：crash 留痕 + 8042 复位（不返回）。
 * Emergency Pool 耗尽 / 双槽皆坏 / 无任务可切换时调用。 */
void drr_system_rollback(const char *reason);

/* 统一 fault 入口：report_fault → 页级回滚 → sched fault 回调（可能不
 * 返回）；回调缺失且 reboot 使能时 → system rollback。 */
void drr_handle_task_fault(u32 task_slot, const char *reason);

/* 看门狗（独立于调度器：由 tick handler 直接调用 check，非调度任务） */
#define DRR_WATCHDOG_MAX 8
int  drr_watchdog_register(u32 task_slot, u32 timeout_ticks);
int  drr_watchdog_unregister(u32 task_slot);
void drr_watchdog_kick(u32 task_slot);
void drr_watchdog_check(void);

/* 恢复行为控制：关闭后 fault 只留日志不真重启（selftest 用） */
void drr_set_reboot_enabled(int enabled);

/* 调度器 fault 回调（sched enable 时注入；缺失时 fault → system rollback）。
 * DRR 独立性：对调度器只依赖这一个函数指针。 */
void drr_set_sched_fault_cb(void (*cb)(u32 task_slot));

#endif /* UTSM_DRR_H */
