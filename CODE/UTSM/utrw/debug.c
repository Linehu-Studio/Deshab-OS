#include <utsm/utrw.h>
#include <utsm/segment.h>
#include <utsm/pckc.h>
#include <utsm/crypto.h>
#include <utsm/log.h>
#include <utsm/panic.h>

void *memcpy(void *dst, const void *src, usize len);

/* ===================================================================
 *  U4: 密文页 dump 工具
 *
 *  "发疯也要可调试。"
 *  对指定段页输出 密文 hex + 解密后明文 hex 对照——开发者可直接
 *  在串口日志核对加密正确性（密文 != 明文、重读一致）。
 *
 *  内核调试特权路径：不经过 capability 校验（这正是工具的意义），
 *  密钥经 PCKC 直接派生。仅调试用，不进任何热路径。
 * =================================================================== */

static void hexdump_line(const char *tag, u64 index, const u8 *data, u64 len) {
    char buf[8 + 2 + 3 * 8 + 1];
    /* "ssssssss: xx xx xx xx xx xx xx xx" */
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        u8 nib = (u8)((index >> ((7 - i) * 4)) & 0xF);
        buf[i] = hexd[nib];
    }
    buf[8] = ':';
    for (u64 b = 0; b < 8; b++) {
        buf[9 + b * 3] = ' ';
        u8 v = (b < len) ? data[b] : 0;
        buf[10 + b * 3] = hexd[(v >> 4) & 0xF];
        buf[11 + b * 3] = hexd[v & 0xF];
    }
    buf[9 + 8 * 3] = 0;
    log_info(tag);
    log_info(buf);
}

int utrw_debug_dump_page(u32 segment_slot, u64 page_index, u64 offset_in_page) {
    utsm_segment_desc *desc = utsm_get_segment(segment_slot);
    if (!desc || desc->state != UTSM_SEG_ACTIVE) {
        return UTSM_ERR_INVALID;
    }
    u64 page_off = page_index * UTSM_PAGE_SIZE;
    if (page_off + UTSM_PAGE_SIZE > desc->cipher_length ||
        offset_in_page + 8 > UTSM_PAGE_SIZE) {
        return UTSM_ERR_BOUNDS;
    }

    log_info("[UTRW] dump: segment");
    log_hex64("[UTRW]   slot=", segment_slot);
    log_hex64("[UTRW]   page=", page_index);
    log_hex64("[UTRW]   offset=", offset_in_page);

    const u8 *cipher = desc->cipher_base + page_off + offset_in_page;
    const u64 SHOW = 64; /* 64B（1 cache line）足以核对 */

    /* ---- 密文 hex ---- */
    for (u64 off = 0; off < SHOW; off += 8) {
        hexdump_line("[UTRW]   cipher=", page_off + offset_in_page + off, cipher + off, 8);
    }

    /* ---- 明文 hex（内核调试特权解密：PCKC 派生 + line 解密） ---- */
    utsm_key_material key;
    int st = utsm_pckc_get_or_derive(segment_slot, desc, &key);
    if (st != UTSM_OK) {
        log_warn("[UTRW] dump: key derive failed");
        panic_full("UTRW-E10 DUMP KEY DERIVE FAILED",
                   "utrw_debug_dump_page: PCKC key derivation failed", 0);
        return st;
    }
    u64 dump_line = (page_off + offset_in_page) / UTSM_CACHE_LINE_SIZE;
    u8 plain[UTSM_CACHE_LINE_SIZE];
    {
        u64 tweak = utsm_make_tweak(desc->tweak_seed, dump_line, desc->key_epoch);
        utsm_crypt_line(&key, tweak,
                        desc->cipher_base + dump_line * UTSM_CACHE_LINE_SIZE, plain);
    }
    u64 in_line = (page_off + offset_in_page) % UTSM_CACHE_LINE_SIZE;
    for (u64 off = 0; off < SHOW; off += 8) {
        u64 src = in_line + off;
        if (src >= UTSM_CACHE_LINE_SIZE) break;
        hexdump_line("[UTRW]   plain= ", page_off + offset_in_page + off,
                     plain + src, 8);
    }

    log_info("[UTRW] dump done");
    return UTSM_OK;
}
