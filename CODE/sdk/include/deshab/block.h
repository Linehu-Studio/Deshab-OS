/* deshab/block.h - block provider 便捷访问层
 *
 * 封装从 dkm_kernel_api 获取 block API 并调用 read/write 的样板代码，
 * 消除散落在各应用的硬编码偏移（+0xA8 / +0x10 / +0x18）。
 *
 * 用法：
 *   dsb_block blk;
 *   if (dsb_block_init(&blk, ctx->dkm_kernel_api) == 0) {
 *       dsb_block_read(&blk, 0, 256, buf);
 *       dsb_block_write(&blk, 0, 1, buf);
 *   }
 */
#ifndef DESHAB_BLOCK_H
#define DESHAB_BLOCK_H

#include "types.h"
#include "kernel_api.h"

typedef struct dsb_block {
    const dkm_block_api *api;
    /* 便捷函数指针（从 block_api 解析）。index 固定 0，与现有应用一致。 */
    int (*read)(u32 index, u64 lba, u32 count, void *buffer);
    int (*write)(u32 index, u64 lba, u32 count, const void *buffer);
} dsb_block;

/* 从 kernel_api 指针初始化 block 便捷结构。
 * kernel_api_u64 为 dsk_boot_context.dkm_kernel_api 的值（u64 指针）。
 * 返回 0 成功，-1 无 block API。 */
static inline int dsb_block_init(dsb_block *b, u64 kernel_api_u64) {
    b->api = 0;
    b->read = 0;
    b->write = 0;
    if (!kernel_api_u64) return -1;
    /* block API 指针位于 kernel_api + DSB_KAPI_OFF_BLOCK */
    const dkm_block_api *blk_api = *(const dkm_block_api **)(kernel_api_u64 + DSB_KAPI_OFF_BLOCK);
    if (!blk_api) return -1;
    b->api = blk_api;
    /* read @ +0x10, write @ +0x18（dkm_block_api 结构内偏移） */
    b->read  = (int (*)(u32, u64, u32, void *))(*(u64 *)((u64)blk_api + 0x10));
    b->write = (int (*)(u32, u64, u32, const void *))(*(u64 *)((u64)blk_api + 0x18));
    return 0;
}

/* 便捷读写（index 固定 0，与现有应用一致）。返回 0 成功。 */
static inline int dsb_block_read(dsb_block *b, u64 lba, u32 count, void *buf) {
    return b->read ? b->read(0, lba, count, buf) : -1;
}

static inline int dsb_block_write(dsb_block *b, u64 lba, u32 count, const void *buf) {
    return b->write ? b->write(0, lba, count, buf) : -1;
}

#endif /* DESHAB_BLOCK_H */
