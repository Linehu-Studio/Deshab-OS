/* stream.c — 64B cache line 流加密（Phase 9：ChaCha20）
 *
 * ct = pt XOR ChaCha20_block(segment_key, nonce=tweak, counter=0)
 * 1 block = 1 line；加密解密同函数（XOR 流）。
 * nonce 唯一性：(segment_key 全局唯一) × (line_index 段内唯一)，
 * key_epoch 混入 key 派生，故同 (key,nonce) 只生成一次 keystream。
 */

#include <utsm/crypto.h>
#include <utsm/blake2b.h>
#include <utsm/config.h>

void utsm_crypt_line(const utsm_key_material *key, u64 tweak, const u8 *in, u8 *out) {
    u32 kw[8];
    for (int i = 0; i < 4; i++) {
        kw[2 * i] = (u32)(key->words[i] & 0xFFFFFFFFu);
        kw[2 * i + 1] = (u32)(key->words[i] >> 32);
    }

    u8 ks[64];
    utsm_chacha20_block(kw, tweak, 0, ks);

    for (u32 i = 0; i < UTSM_CACHE_LINE_SIZE; i++) {
        out[i] = in[i] ^ ks[i];
    }
}
