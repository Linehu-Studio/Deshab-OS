#include <utsm/utrw.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/crypto.h>
#include <utsm/config.h>

void *memcpy(void *dst, const void *src, usize len);

int utsm_read(utsm_capability cap, u64 offset, void *dst, u64 len) {
    if (!dst && len != 0) {
        return UTSM_ERR_INVALID;
    }
    if (len == 0) {
        return UTSM_OK;
    }

    utsm_segment_desc *desc = NULL;
    int status = utsm_cap_validate(cap, UTSM_RIGHT_READ, offset, len, &desc);
    if (status != UTSM_OK) {
        return status;
    }

    /* U3: @unsealed 段 —— 明文直存，0 加解密开销（capability 校验照走） */
    if (desc->flags & UTSM_SEG_F_UNSEALED) {
        memcpy(dst, desc->cipher_base + offset, len);
        return UTSM_OK;
    }

    utsm_key_material key;
    status = utsm_pckc_get_or_derive(cap.segment_slot, desc, &key);
    if (status != UTSM_OK) {
        return status;
    }

    u8 line_plain[UTSM_CACHE_LINE_SIZE];
    u64 seq_before = desc->writer_seq;
    if (seq_before & 1ULL) {
        return UTSM_ERR_RETRY;
    }

    u64 copied = 0;
    while (copied < len) {
        u64 absolute = offset + copied;
        u64 line_start = (absolute / UTSM_CACHE_LINE_SIZE) * UTSM_CACHE_LINE_SIZE;
        u64 in_line = absolute - line_start;
        u64 chunk = UTSM_CACHE_LINE_SIZE - in_line;
        if (chunk > len - copied) {
            chunk = len - copied;
        }
        u64 line_index = line_start / UTSM_CACHE_LINE_SIZE;
        u64 tweak = utsm_make_tweak(desc->tweak_seed, line_index, desc->key_epoch);
        utsm_crypt_line(&key, tweak, desc->cipher_base + line_start, line_plain);
        memcpy((u8 *)dst + copied, line_plain + in_line, chunk);
        copied += chunk;
    }

    u64 seq_after = desc->writer_seq;
    if (seq_before != seq_after) {
        return UTSM_ERR_RETRY;
    }
    return UTSM_OK;
}
