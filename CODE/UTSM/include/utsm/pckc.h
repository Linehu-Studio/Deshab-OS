#ifndef UTSM_PCKC_H
#define UTSM_PCKC_H

#include <utsm/types.h>
#include <utsm/config.h>
#include <utsm/uuid.h>
#include <utsm/crypto.h>

struct utsm_segment_desc;

typedef struct {
    bool valid;
    u32 segment_slot;
    u64 key_epoch;
    utsm_key_material key;
} utsm_pckc_entry;

typedef struct {
    uuid128_t current_process_uuid;
    utsm_pckc_entry entries[UTSM_MAX_PCKC_KEYS];
    u64 hit_count;
    u64 miss_count;
    u32 next_replace;
} utsm_pckc;

void utsm_pckc_init(void);
int utsm_pckc_get_or_derive(u32 segment_slot, struct utsm_segment_desc *desc, utsm_key_material *out_key);

#endif
