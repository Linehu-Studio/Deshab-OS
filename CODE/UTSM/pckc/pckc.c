#include <utsm/pckc.h>
#include <utsm/segment.h>
#include <utsm/config.h>

static utsm_pckc g_pckc;

void utsm_pckc_init(void) {
    g_pckc.current_process_uuid = uuid_make_test(0);
    g_pckc.hit_count = 0;
    g_pckc.miss_count = 0;
    g_pckc.next_replace = 0;
    for (u32 i = 0; i < UTSM_MAX_PCKC_KEYS; i++) {
        g_pckc.entries[i].valid = false;
    }
}

int utsm_pckc_get_or_derive(u32 segment_slot, utsm_segment_desc *desc, utsm_key_material *out_key) {
    for (u32 i = 0; i < UTSM_MAX_PCKC_KEYS; i++) {
        utsm_pckc_entry *entry = &g_pckc.entries[i];
        if (entry->valid && entry->segment_slot == segment_slot && entry->key_epoch == desc->key_epoch) {
            *out_key = entry->key;
            g_pckc.hit_count++;
            return UTSM_OK;
        }
    }

    g_pckc.miss_count++;
    u32 idx = g_pckc.next_replace++ % UTSM_MAX_PCKC_KEYS;
    utsm_pckc_entry *entry = &g_pckc.entries[idx];
    entry->valid = true;
    entry->segment_slot = segment_slot;
    entry->key_epoch = desc->key_epoch;
    utsm_kdf_segment_key(desc->owner_process_uuid, desc->segment_uuid, desc->key_epoch, &entry->key);
    *out_key = entry->key;
    return UTSM_OK;
}

void utsm_pckc_invalidate_slot(u32 segment_slot) {
    for (u32 i = 0; i < UTSM_MAX_PCKC_KEYS; i++) {
        utsm_pckc_entry *entry = &g_pckc.entries[i];
        if (entry->valid && entry->segment_slot == segment_slot) {
            entry->valid = false;
            entry->key = (utsm_key_material){ { 0, 0, 0, 0 } };  /* 擦除密钥材料 */
        }
    }
}
