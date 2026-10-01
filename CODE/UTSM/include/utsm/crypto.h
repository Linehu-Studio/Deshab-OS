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

/* ===================================================================
 *  Phase 9 扩展：真实加密原语服务（尾部追加，ABI 兼容）
 *  stream = ChaCha20 block（RFC 8439），hash/KDF/MAC = BLAKE2b（RFC 7693）
 * =================================================================== */

/* ChaCha20 block function：64B keystream。
 * counter=0 用法下 nonce=tweak 即可保证 (key,nonce) 唯一。 */
void utsm_chacha20_block(const u32 key_words[8], u64 nonce, u64 counter, u8 out[64]);

/* 4KB 密文页 MAC：keyed BLAKE2b(key=root_key, domain||seg_slot||page||epoch||data)，
 * 截断 64 位。root_key 为 4×u64（DRR root key）。 */
u64 utsm_page_mac(const u64 *root_key, u32 seg_slot, u64 page_index, u64 key_epoch, const void *page);

#endif
