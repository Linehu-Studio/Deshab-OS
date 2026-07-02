#include <utsm/capability.h>
#include <utsm/segment.h>

int utsm_cap_validate(utsm_capability cap, u32 required_rights, u64 offset, u64 len, utsm_segment_desc **out_desc) {
    utsm_segment_desc *desc = utsm_get_segment(cap.segment_slot);
    if (!desc) {
        return UTSM_ERR_INVALID;
    }
    if (desc->state == UTSM_SEG_POISONED) {
        return UTSM_ERR_POISONED;
    }
    if (desc->state != UTSM_SEG_ACTIVE) {
        return UTSM_ERR_STATE;
    }
    if (desc->generation != cap.generation) {
        return UTSM_ERR_STALE_CAP;
    }
    if ((cap.rights & required_rights) != required_rights) {
        return UTSM_ERR_ACCESS;
    }
    if ((u32)desc->key_epoch != cap.epoch) {
        return UTSM_ERR_EPOCH;
    }
    if (offset > desc->cipher_length || len > desc->cipher_length || offset + len > desc->cipher_length) {
        return UTSM_ERR_BOUNDS;
    }
    *out_desc = desc;
    return UTSM_OK;
}
