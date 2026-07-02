#include <utsm/dirty.h>
#include <utsm/segment.h>
#include <utsm/arena.h>
#include <utsm/config.h>

static void bitmap_set(u64 *bitmap, u64 bit) {
    bitmap[bit / 64] |= 1ULL << (bit % 64);
}

static bool bitmap_test(u64 *bitmap, u64 bit) {
    return (bitmap[bit / 64] & (1ULL << (bit % 64))) != 0;
}

void utsm_dirty_init(utsm_segment_desc *desc, u64 page_count) {
    desc->dirty_shard_count = 1;
    desc->dirty_shards = (utsm_dirty_shard *)kmem_alloc(sizeof(utsm_dirty_shard));
    if (!desc->dirty_shards) {
        return;
    }
    u64 words = (page_count + 63) / 64;
    desc->dirty_shards[0].dirty_count = 0;
    desc->dirty_shards[0].shard_id = 0;
    desc->dirty_shards[0].page_base = 0;
    desc->dirty_shards[0].page_count = page_count;
    desc->dirty_shards[0].last_checkpoint_epoch = 0;
    desc->dirty_shards[0].bitmap = (u64 *)kmem_alloc_aligned(words * sizeof(u64), 8);
    if (desc->dirty_shards[0].bitmap) {
        for (u64 i = 0; i < words; i++) {
            desc->dirty_shards[0].bitmap[i] = 0;
        }
    }
}

void utsm_mark_dirty(utsm_segment_desc *desc, u64 offset, u64 len) {
    if (!desc || !desc->dirty_shards || !desc->dirty_shards[0].bitmap || len == 0) {
        return;
    }
    u64 first = offset / UTSM_PAGE_SIZE;
    u64 last = (offset + len - 1) / UTSM_PAGE_SIZE;
    utsm_dirty_shard *shard = &desc->dirty_shards[0];
    for (u64 page = first; page <= last && page < shard->page_count; page++) {
        if (!bitmap_test(shard->bitmap, page)) {
            bitmap_set(shard->bitmap, page);
            shard->dirty_count++;
        }
    }
}
