/* chacha20.c — ChaCha20 block function（RFC 8439 §2.3）
 *
 * block function = 20 轮纯 ARX（无 message word 吸收；sigma 表仅用于
 * 带明文的加密模式）。UTSM 用途：1 block = 1×64B cache line keystream。
 *   counter = 0（整条 line 只消费一个 block）
 *   nonce   = tweak（低 32 位在前，高 32 位在后，第三字为 0）
 * 纯 32-bit 整数运算，-mno-sse 下可用。
 */

#include <utsm/blake2b.h>
#include <utsm/crypto.h>

static const u32 chacha_iv[4] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
};

static inline u32 rotl32(u32 v, u32 c) {
    return (v << c) | (v >> (32 - c));
}

static void chacha_qr(u32 *v, int a, int b, int c, int d) {
    v[a] += v[b]; v[d] ^= v[a]; v[d] = rotl32(v[d], 16);
    v[c] += v[d]; v[b] ^= v[c]; v[b] = rotl32(v[b], 12);
    v[a] += v[b]; v[d] ^= v[a]; v[d] = rotl32(v[d], 8);
    v[c] += v[d]; v[b] ^= v[c]; v[b] = rotl32(v[b], 7);
}

void utsm_chacha20_block(const u32 key_words[8], u64 nonce, u64 counter, u8 out[64]) {
    u32 st[16];
    u32 v[16];

    st[0] = chacha_iv[0];
    st[1] = chacha_iv[1];
    st[2] = chacha_iv[2];
    st[3] = chacha_iv[3];
    for (int i = 0; i < 8; i++) st[4 + i] = key_words[i];
    st[12] = (u32)counter;
    st[13] = (u32)(counter >> 32);
    st[14] = (u32)nonce;
    st[15] = (u32)(nonce >> 32);

    for (int i = 0; i < 16; i++) v[i] = st[i];

    for (int r = 0; r < 10; r++) {
        /* column rounds */
        chacha_qr(v, 0, 4, 8, 12);
        chacha_qr(v, 1, 5, 9, 13);
        chacha_qr(v, 2, 6, 10, 14);
        chacha_qr(v, 3, 7, 11, 15);
        /* diagonal rounds */
        chacha_qr(v, 0, 5, 10, 15);
        chacha_qr(v, 1, 6, 11, 12);
        chacha_qr(v, 2, 7, 8, 13);
        chacha_qr(v, 3, 4, 9, 14);
    }

    for (int i = 0; i < 16; i++) {
        utsm_store32_le(out + 4 * i, v[i] + st[i]);
    }
}
