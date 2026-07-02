#include <utsm/crypto.h>

static u64 mix64_tweak(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

u64 utsm_make_tweak(u64 tweak_seed, u64 line_index, u64 key_epoch) {
    return mix64_tweak(tweak_seed ^ (line_index * 0x9e3779b97f4a7c15ULL) ^ (key_epoch << 32));
}
