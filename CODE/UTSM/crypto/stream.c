#include <utsm/crypto.h>
#include <utsm/config.h>

static u64 next_stream_word(u64 x) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

void utsm_crypt_line(const utsm_key_material *key, u64 tweak, const u8 *in, u8 *out) {
    u64 state = key->words[0] ^ key->words[1] ^ key->words[2] ^ key->words[3] ^ tweak;
    for (u64 i = 0; i < UTSM_CACHE_LINE_SIZE; i += 8) {
        state = next_stream_word(state + i + 0xa5a5a5a5a5a5a5a5ULL);
        for (u64 j = 0; j < 8; j++) {
            out[i + j] = in[i + j] ^ (u8)(state >> (j * 8));
        }
    }
}
