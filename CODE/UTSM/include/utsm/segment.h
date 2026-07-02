#ifndef UTSM_SEGMENT_H
#define UTSM_SEGMENT_H

#include <utsm/types.h>
#include <utsm/uuid.h>
#include <utsm/status.h>
#include <utsm/capability.h>
#include <utsm/dirty.h>

typedef enum {
    UTSM_SEG_FREE = 0,
    UTSM_SEG_ACTIVE,
    UTSM_SEG_CHECKPOINTING,
    UTSM_SEG_SEALED,
    UTSM_SEG_RECOVERING,
    UTSM_SEG_POISONED,
    UTSM_SEG_DESTROYED
} utsm_segment_state;

#define UTSM_SEG_F_READ            (1U << 0)
#define UTSM_SEG_F_WRITE           (1U << 1)
#define UTSM_SEG_F_EXEC            (1U << 2)
#define UTSM_SEG_F_DMA             (1U << 3)
#define UTSM_SEG_F_STRONG_RECOVERY (1U << 4)

typedef struct utsm_segment_desc {
    uuid128_t segment_uuid;
    uuid128_t owner_process_uuid;
    u8 *cipher_base;
    u64 cipher_length;
    void *dmp_base;
    u64 key_epoch;
    u64 tweak_seed;
    utsm_dirty_shard *dirty_shards;
    u32 dirty_shard_count;
    void *mac_table;
    u32 generation;
    u32 flags;
    u32 state;
    u32 last_cpu;
    volatile u64 writer_seq;
    u64 checkpoint_generation;
} utsm_segment_desc;

struct utsm_process_context;

int utsm_create_segment(struct utsm_process_context *process, u64 size, u32 flags, utsm_capability *out_cap);
utsm_segment_desc *utsm_get_segment(u32 slot);

#endif
