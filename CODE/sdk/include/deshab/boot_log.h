/* deshab/boot_log.h — SDK 侧 boot log 绑定（见 CODE/tools/boot_log.h） */
#ifndef DESHAB_BOOT_LOG_H
#define DESHAB_BOOT_LOG_H

#include "../../tools/boot_log.h"
#include "boot_context.h"

static inline void dsb_bootlog_bind(dsb_boot_log_info **slot, const dsk_boot_context *ctx) {
    boot_log_bind((boot_log_info **)slot, ctx ? ctx->utsm_state : 0);
}

/* 兼容别名 */
typedef boot_log_info dsb_boot_log_info;

#endif /* DESHAB_BOOT_LOG_H */
