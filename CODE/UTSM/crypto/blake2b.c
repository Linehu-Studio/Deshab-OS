/* blake2b.c — BLAKE2b（RFC 7693）实现
 *
 * 12 轮、128B 块、64-bit G 函数（旋转 32/24/16/63）。
 * keyed 模式：init 时将 key 填充为首个 128B 块（RFC 7693 §2.3），
 * update/final 沿用引用实现语义（惰性缓冲 + 计数器含 key 块）。
 */

#include <utsm/blake2b.h>

static const u64 blake2b_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const u8 blake2b_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
    { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
    { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
    { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
    { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
    { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
    { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
    { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
    { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 }
};

static inline u64 rotr64(u64 v, u32 c) {
    return (v >> c) | (v << (64 - c));
}

#define B2B_G(v, a, b, c, d, x, y) do {                      \
    (v)[a] = (v)[a] + (v)[b] + (x); (v)[d] = rotr64((v)[d] ^ (v)[a], 32); \
    (v)[c] = (v)[c] + (v)[d];       (v)[b] = rotr64((v)[b] ^ (v)[c], 24); \
    (v)[a] = (v)[a] + (v)[b] + (y); (v)[d] = rotr64((v)[d] ^ (v)[a], 16); \
    (v)[c] = (v)[c] + (v)[d];       (v)[b] = rotr64((v)[b] ^ (v)[c], 63); \
} while (0)

static void blake2b_compress(utsm_blake2b_ctx *ctx, const u8 *block) {
    u64 m[16];
    u64 v[16];

    for (int i = 0; i < 16; i++) {
        m[i] = utsm_load64_le(block + 8 * i);
    }
    for (int i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = blake2b_iv[i];
    }
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (ctx->last_block) {
        v[14] ^= ~(u64)0;
    }

    for (int r = 0; r < 12; r++) {
        const u8 *s = blake2b_sigma[r % 10];
        B2B_G(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        B2B_G(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        B2B_G(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        B2B_G(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        B2B_G(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        B2B_G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        B2B_G(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        B2B_G(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }

    for (int i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

void utsm_blake2b_init(utsm_blake2b_ctx *ctx, u32 out_len, const void *key, u32 key_len) {
    if (out_len == 0 || out_len > UTSM_BLAKE2B_MAXOUT) out_len = UTSM_BLAKE2B_MAXOUT;
    if (key_len > 64u) key_len = 64u;

    for (int i = 0; i < 8; i++) ctx->h[i] = blake2b_iv[i];
    ctx->h[0] ^= 0x01010000ULL ^ ((u64)key_len << 8) ^ (u64)out_len;
    ctx->t[0] = 0;
    ctx->t[1] = 0;
    ctx->out_len = out_len;
    ctx->buf_len = 0;
    ctx->last_block = 0;

    if (key && key_len > 0u) {
        const u8 *k = (const u8 *)key;
        for (u32 i = 0; i < key_len; i++) ctx->buf[i] = k[i];
        for (u32 i = key_len; i < 128u; i++) ctx->buf[i] = 0;
        ctx->buf_len = 128u;    /* key 块挂起，随首个 update/final 压缩 */
    }
}

void utsm_blake2b_update(utsm_blake2b_ctx *ctx, const void *data, u64 len) {
    const u8 *in = (const u8 *)data;

    while (len > 0) {
        u32 left = ctx->buf_len;
        u32 fill = UTSM_BLAKE2B_BLOCKBYTES - left;

        if (len > (u64)fill) {
            for (u32 i = 0; i < fill; i++) ctx->buf[left + i] = in[i];
            ctx->t[0] += 128;
            if (ctx->t[0] < 128) ctx->t[1]++;
            blake2b_compress(ctx, ctx->buf);
            ctx->buf_len = 0;
            in += fill;
            len -= fill;
        } else {
            for (u64 i = 0; i < len; i++) ctx->buf[left + i] = in[i];
            ctx->buf_len += (u32)len;
            return;
        }
    }
}

void utsm_blake2b_final(utsm_blake2b_ctx *ctx, void *out) {
    ctx->t[0] += ctx->buf_len;
    if (ctx->t[0] < (u64)ctx->buf_len) ctx->t[1]++;
    ctx->last_block = 1;

    for (u32 i = ctx->buf_len; i < UTSM_BLAKE2B_BLOCKBYTES; i++) ctx->buf[i] = 0;
    blake2b_compress(ctx, ctx->buf);

    u8 tmp[64];
    for (int i = 0; i < 8; i++) utsm_store64_le(tmp + 8 * i, ctx->h[i]);
    u8 *o = (u8 *)out;
    for (u32 i = 0; i < ctx->out_len; i++) o[i] = tmp[i];
}

void utsm_blake2b(const void *data, u64 len, const void *key, u32 key_len, void *out, u32 out_len) {
    utsm_blake2b_ctx ctx;
    utsm_blake2b_init(&ctx, out_len, key, key_len);
    utsm_blake2b_update(&ctx, data, len);
    utsm_blake2b_final(&ctx, out);
}
