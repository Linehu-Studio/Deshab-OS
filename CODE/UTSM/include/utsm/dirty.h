#ifndef UTSM_DIRTY_H
#define UTSM_DIRTY_H

#include <utsm/types.h>

typedef struct {
    u32 dirty_count;
    u32 shard_id;
    u64 page_base;
    u64 page_count;
    u64 last_checkpoint_epoch;
    u64 *bitmap;
} utsm_dirty_shard;

struct utsm_segment_desc;

void utsm_dirty_init(struct utsm_segment_desc *desc, u64 page_count);
void utsm_mark_dirty(struct utsm_segment_desc *desc, u64 offset, u64 len);

#endif
