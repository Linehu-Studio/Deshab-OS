#include <utsm/segment.h>
#include <utsm/process.h>
#include <utsm/arena.h>
#include <utsm/dmp.h>
#include <utsm/config.h>

u64 utsm_next_generation(void);

static u64 digest_uuid(uuid128_t id) {
    return id.hi ^ id.lo ^ (id.hi >> 32) ^ (id.lo << 17);
}

static u64 make_tweak_seed(uuid128_t process_uuid, uuid128_t segment_uuid, u64 epoch) {
    return process_uuid.hi ^ process_uuid.lo ^ segment_uuid.hi ^ segment_uuid.lo ^ (epoch * 0x9e3779b97f4a7c15ULL);
}

int utsm_create_segment(utsm_process_context *process, u64 size, u32 flags, utsm_capability *out_cap) {
    if (!process || !out_cap || size == 0) {
        return UTSM_ERR_INVALID;
    }

    u32 slot = UTSM_MAX_SEGMENTS;
    utsm_segment_desc *desc = NULL;
    for (u32 i = 0; i < UTSM_MAX_SEGMENTS; i++) {
        utsm_segment_desc *candidate = utsm_get_segment(i);
        if (candidate->state == UTSM_SEG_FREE) {
            slot = i;
            desc = candidate;
            break;
        }
    }
    if (!desc) {
        return UTSM_ERR_NO_MEMORY;
    }

    u64 length = utsm_align_up_u64(size, UTSM_CACHE_LINE_SIZE);
    u64 pages = utsm_align_up_u64(length, UTSM_PAGE_SIZE) / UTSM_PAGE_SIZE;
    u8 *cipher = (u8 *)kmem_alloc_page(pages);
    utsm_dmp *dmp = (utsm_dmp *)kmem_alloc_aligned(sizeof(utsm_dmp), 64);
    void *mac_table = kmem_alloc_aligned(pages * sizeof(u64), 8);
    if (!cipher || !dmp || !mac_table) {
        return UTSM_ERR_NO_MEMORY;
    }

    for (u64 i = 0; i < pages * UTSM_PAGE_SIZE; i++) {
        cipher[i] = 0;
    }

    desc->segment_uuid = uuid_make_test(utsm_next_generation() + slot);
    desc->owner_process_uuid = process->process_uuid;
    desc->cipher_base = cipher;
    desc->cipher_length = length;
    desc->dmp_base = dmp;
    desc->key_epoch = process->crypto_epoch;
    desc->tweak_seed = make_tweak_seed(process->process_uuid, desc->segment_uuid, desc->key_epoch);
    desc->mac_table = mac_table;
    desc->generation++;
    desc->flags = flags;
    desc->state = UTSM_SEG_ACTIVE;
    desc->last_cpu = 0;
    desc->writer_seq = 0;
    desc->checkpoint_generation = process->recovery_generation;

    utsm_dirty_init(desc, pages);
    utsm_dmp_init(dmp, desc, slot);

    out_cap->segment_slot = slot;
    out_cap->generation = desc->generation;
    out_cap->rights = 0;
    if (flags & UTSM_SEG_F_READ) out_cap->rights |= UTSM_RIGHT_READ;
    if (flags & UTSM_SEG_F_WRITE) out_cap->rights |= UTSM_RIGHT_WRITE;
    if (flags & UTSM_SEG_F_EXEC) out_cap->rights |= UTSM_RIGHT_EXEC;
    if (flags & UTSM_SEG_F_DMA) out_cap->rights |= UTSM_RIGHT_DMA;
    out_cap->epoch = (u32)desc->key_epoch;
    out_cap->uuid_digest = digest_uuid(desc->segment_uuid);
    out_cap->auth_tag = out_cap->uuid_digest ^ digest_uuid(process->process_uuid) ^ desc->key_epoch;

    process->capability_count++;
    return UTSM_OK;
}
