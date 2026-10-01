#ifndef UTSM_BLAKE2B_H
#define UTSM_BLAKE2B_H

#include <utsm/types.h>

/* ===================================================================
 *  BLAKE2b（RFC 7693）— Phase 9 真实加密原语
 *
 *  纯 64-bit 整数实现（无 SSE/FPU 依赖，-mno-sse 下可用）。
 *  支持 1-64B 可变输出与 keyed 模式（key 0-64B）。
 *  角色：KDF（root→segment key）、页 MAC、mac_root 链。
 * =================================================================== */

#define UTSM_BLAKE2B_BLOCKBYTES 128u
#define UTSM_BLAKE2B_MAXOUT     64u

typedef struct {
    u64 h[8];               /* 链值 */
    u64 t[2];               /* 已处理字节数（128 位计数器） */
    u8  buf[128];           /* 待压缩块缓冲 */
    u32 buf_len;            /* buf 内有效字节数 */
    u32 out_len;            /* 摘要输出长度 1-64 */
    u32 last_block;         /* 最后一块标志 */
} utsm_blake2b_ctx;

void utsm_blake2b_init(utsm_blake2b_ctx *ctx, u32 out_len, const void *key, u32 key_len);
void utsm_blake2b_update(utsm_blake2b_ctx *ctx, const void *data, u64 len);
void utsm_blake2b_final(utsm_blake2b_ctx *ctx, void *out);

/* one-shot：out_len 1-64，key_len 0-64（key=NULL 表示无 key） */
void utsm_blake2b(const void *data, u64 len, const void *key, u32 key_len, void *out, u32 out_len);

/* ---- 小端序列化辅助（crypto 各处共用） ---- */
static inline void utsm_store32_le(u8 *p, u32 v) {
    p[0] = (u8)(v); p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static inline u32 utsm_load32_le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline void utsm_store64_le(u8 *p, u64 v) {
    utsm_store32_le(p, (u32)v);
    utsm_store32_le(p + 4, (u32)(v >> 32));
}
static inline u64 utsm_load64_le(const u8 *p) {
    return (u64)utsm_load32_le(p) | ((u64)utsm_load32_le(p + 4) << 32);
}

#endif /* UTSM_BLAKE2B_H */
