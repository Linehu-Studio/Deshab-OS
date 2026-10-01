/* kdf.c — 段密钥派生（Phase 9：keyed BLAKE2b）
 *
 * segment_key = BLAKE2b-256(key=root_key, "UTSMSEGK"||p_uuid||s_uuid||key_epoch)
 * root key 不直接加密数据；UUID 作为派生调谐因子（非明文密钥）。
 * 确定性：同 (puuid, suuid, epoch) 恒得同 key —— PCKC 缓存正确性依赖此性质。
 */

#include <utsm/crypto.h>
#include <utsm/blake2b.h>
#include <utsm/drr.h>

void utsm_kdf_segment_key(uuid128_t process_uuid, uuid128_t segment_uuid, u64 key_epoch, utsm_key_material *out_key) {
    const u64 *root = drr_get_root_key();

    u8 root_le[32];
    u8 data[8 + 8 + 8 + 8 + 8];
    u8 digest[32];

    for (int i = 0; i < 4; i++) utsm_store64_le(root_le + 8 * i, root[i]);

    utsm_store64_le(data + 0, 0x4B4745534D535455ULL);   /* "UTSMSEGK" LE */
    utsm_store64_le(data + 8, process_uuid.hi);
    utsm_store64_le(data + 16, process_uuid.lo);
    utsm_store64_le(data + 24, segment_uuid.hi);
    utsm_store64_le(data + 32, segment_uuid.lo);
    utsm_store64_le(data + 40, key_epoch);

    utsm_blake2b(data, sizeof(data), root_le, 32, digest, 32);

    for (int i = 0; i < 4; i++) out_key->words[i] = utsm_load64_le(digest + 8 * i);
}
