#ifndef UTSM_CRYPTO_H
#define UTSM_CRYPTO_H

#include <utsm/types.h>
#include <utsm/uuid.h>

struct utsm_segment_desc;

typedef struct {
    u64 words[4];
} utsm_key_material;

void utsm_kdf_segment_key(uuid128_t process_uuid, uuid128_t segment_uuid, u64 key_epoch, utsm_key_material *out_key);
u64 utsm_make_tweak(u64 tweak_seed, u64 line_index, u64 key_epoch);
void utsm_crypt_line(const utsm_key_material *key, u64 tweak, const u8 *in, u8 *out);

#endif
