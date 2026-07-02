#include <utsm/utrw.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/crypto.h>
#include <utsm/config.h>
#include <utsm/drr.h>

void *memcpy(void *dst, const void *src, usize len);

int utsm_write(utsm_capability cap, u64 offset, const void *src, u64 len) {
    if (!src && len != 0) {
        return UTSM_ERR_INVALID;
    }
    if (len == 0) {
        return UTSM_OK;
    }

    utsm_segment_desc *desc = NULL;
    int status = utsm_cap_validate(cap, UTSM_RIGHT_WRITE, offset, len, &desc);
    if (status != UTSM_OK) {
        return status;
    }

    utsm_key_material key;
    status = utsm_pckc_get_or_derive(cap.segment_slot, desc, &key);
    if (status != UTSM_OK) {
        return status;
    }

    drr_log_write_intent(cap.segment_slot, offset, len);
    desc->writer_seq++;

    u8 line_plain[UTSM_CACHE_LINE_SIZE];
    u8 line_cipher[UTSM_CACHE_LINE_SIZE];
    u64 written = 0;
    while (written < len) {
        u64 absolute = offset + written;
        u64 line_start = (absolute / UTSM_CACHE_LINE_SIZE) * UTSM_CACHE_LINE_SIZE;
        u64 in_line = absolute - line_start;
        u64 chunk = UTSM_CACHE_LINE_SIZE - in_line;
        if (chunk > len - written) {
            chunk = len - written;
        }
        u64 line_index = line_start / UTSM_CACHE_LINE_SIZE;
        u64 tweak = utsm_make_tweak(desc->tweak_seed, line_index, desc->key_epoch);

        if (chunk == UTSM_CACHE_LINE_SIZE && in_line == 0) {
            memcpy(line_plain, (const u8 *)src + written, UTSM_CACHE_LINE_SIZE);
        } else {
            utsm_crypt_line(&key, tweak, desc->cipher_base + line_start, line_plain);
            memcpy(line_plain + in_line, (const u8 *)src + written, chunk);
        }

        utsm_crypt_line(&key, tweak, line_plain, line_cipher);
        memcpy(desc->cipher_base + line_start, line_cipher, UTSM_CACHE_LINE_SIZE);
        written += chunk;
    }

    utsm_mark_dirty(desc, offset, len);
    desc->writer_seq++;
    drr_log_write_commit(cap.segment_slot, offset, len);
    return UTSM_OK;
}
