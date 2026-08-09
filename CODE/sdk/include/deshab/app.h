/* deshab/app.h - 应用上下文与初始化
 *
 * 整合 desktop_app.h 的 da_app_context / da_init，提供从 dsk_boot_context
 * 提取常用数据（framebuffer + block + kernel_api）的便捷封装。
 *
 * 应用入口模式：
 *   void dsk_entry(const dsk_boot_context *ctx) {
 *       dsb_app_context ac;
 *       dsb_app_init(&ac, ctx);
 *       // 使用 ac.fb / ac.block / ac.api ...
 *   }
 */
#ifndef DESHAB_APP_H
#define DESHAB_APP_H

#include "types.h"
#include "boot_context.h"
#include "kernel_api.h"
#include "block.h"

typedef struct dsb_app_context {
    u32 *fb;                    /* framebuffer 基址（32bpp BGRA） */
    u64  fb_w;                  /* 宽度（像素） */
    u64  fb_h;                  /* 高度（像素） */
    u64  fb_pitch;              /* 行跨度（字节） */
    dsb_block block;            /* block 便捷读写 */
    const dkm_kernel_api *api;  /* kernel_api 指针 */
} dsb_app_context;

/* 从 dsk_boot_context 初始化 dsb_app_context。
 * block API 从 kernel_api + DSB_KAPI_OFF_BLOCK 获取（封装在 dsb_block_init）。 */
static inline void dsb_app_init(dsb_app_context *ac, const dsk_boot_context *ctx) {
    ac->fb = (u32 *)ctx->framebuffer_address;
    ac->fb_w = ctx->framebuffer_width;
    ac->fb_h = ctx->framebuffer_height;
    ac->fb_pitch = ctx->framebuffer_pitch;
    ac->api = (const dkm_kernel_api *)ctx->dkm_kernel_api;
    dsb_block_init(&ac->block, ctx->dkm_kernel_api);
}

#endif /* DESHAB_APP_H */
