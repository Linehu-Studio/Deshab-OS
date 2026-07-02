#ifndef UTSM_DMP_H
#define UTSM_DMP_H

#include <utsm/types.h>

typedef struct {
    u64 magic;
    u32 version;
    u32 segment_slot;
    u64 fast_base;
    u64 fast_limit;
    u64 fast_tweak_seed;
    u64 fast_key_epoch;
    u64 dirty_bitmap_base;
    u64 mac_table_base;
    u64 last_writer_seq;
    u32 last_cpu;
    u32 flags;
    u64 crc;
} utsm_dmp;

struct utsm_segment_desc;

void utsm_dmp_init(utsm_dmp *dmp, struct utsm_segment_desc *desc, u32 slot);

#endif
