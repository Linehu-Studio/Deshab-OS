#include <utsm/dmp.h>
#include <utsm/segment.h>

#define UTSM_DMP_MAGIC 0x5554534d444d5031ULL

void utsm_dmp_init(utsm_dmp *dmp, utsm_segment_desc *desc, u32 slot) {
    dmp->magic = UTSM_DMP_MAGIC;
    dmp->version = 1;
    dmp->segment_slot = slot;
    dmp->fast_base = (u64)desc->cipher_base;
    dmp->fast_limit = (u64)desc->cipher_base + desc->cipher_length;
    dmp->fast_tweak_seed = desc->tweak_seed;
    dmp->fast_key_epoch = desc->key_epoch;
    dmp->dirty_bitmap_base = desc->dirty_shards ? (u64)desc->dirty_shards[0].bitmap : 0;
    dmp->mac_table_base = (u64)desc->mac_table;
    dmp->last_writer_seq = desc->writer_seq;
    dmp->last_cpu = desc->last_cpu;
    dmp->flags = desc->flags;
    dmp->crc = dmp->magic ^ dmp->fast_base ^ dmp->fast_limit ^ dmp->fast_tweak_seed;
}
