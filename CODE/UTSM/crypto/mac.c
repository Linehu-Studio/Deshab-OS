/* mac.c — 4KB 密文页 MAC（Phase 9：keyed BLAKE2b）
 *
 * page_mac = BLAKE2b-256(key=root_key, "UTSMPAGE"||seg_slot||page_index||key_epoch||cipher_page)[0..8)
 * 绑定页归属（段/页号/纪元），防密文页在段间移位或跨纪元重放。
 * 用途：DRR checkpoint 快照完整性 / rollback 校验（替换 CRC64 占位 MAC）。
 */

#include <utsm/crypto.h>
#include <utsm/blake2b.h>
#include <utsm/config.h>

u64 utsm_page_mac(const u64 *root_key, u32 seg_slot, u64 page_index, u64 key_epoch, const void *page) {
    u8 root_le[32];
    u8 ctx_data[8 + 4 + 4 + 8 + 8];
    u8 digest[32];

    for (int i = 0; i < 4; i++) utsm_store64_le(root_le + 8 * i, root_key[i]);

    utsm_store64_le(ctx_data + 0, 0x454741504D535455ULL);   /* "UTSMPAGE" LE */
    utsm_store32_le(ctx_data + 8, seg_slot);
    utsm_store32_le(ctx_data + 12, 0);
    utsm_store64_le(ctx_data + 16, page_index);
    utsm_store64_le(ctx_data + 24, key_epoch);

    utsm_blake2b_ctx c;
    utsm_blake2b_init(&c, 32, root_le, 32);
    utsm_blake2b_update(&c, ctx_data, sizeof(ctx_data));
    utsm_blake2b_update(&c, page, UTSM_PAGE_SIZE);
    utsm_blake2b_final(&c, digest);

    return utsm_load64_le(digest);
}
