#include <utsm/crypto.h>
#include <utsm/drr.h>

static u64 mix64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

void utsm_kdf_segment_key(uuid128_t process_uuid, uuid128_t segment_uuid, u64 key_epoch, utsm_key_material *out_key) {
    const u64 *root = drr_get_root_key();
    out_key->words[0] = mix64(root[0] ^ process_uuid.hi ^ segment_uuid.lo ^ key_epoch);
    out_key->words[1] = mix64(root[1] ^ process_uuid.lo ^ segment_uuid.hi ^ (key_epoch << 1));
    out_key->words[2] = mix64(root[2] ^ process_uuid.hi ^ segment_uuid.hi ^ (key_epoch << 7));
    out_key->words[3] = mix64(root[3] ^ process_uuid.lo ^ segment_uuid.lo ^ (key_epoch << 13));
}
