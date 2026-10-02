#ifndef UTSM_UTRW_H
#define UTSM_UTRW_H

#include <utsm/types.h>
#include <utsm/status.h>
#include <utsm/capability.h>

int utsm_read(utsm_capability cap, u64 offset, void *dst, u64 len);
int utsm_write(utsm_capability cap, u64 offset, const void *src, u64 len);

/* U2: Slow Path —— Fast Path 失败后的恢复分支。
 * reason 为 Fast Path 返回的错误码；恢复成功返回 UTSM_OK（可重试），
 * EPOCH 场景经 out_cap 回传刷新后的凭证（可为 NULL）。
 * 其余返回值为不可恢复错误。 */
int utsm_slow_path(utsm_capability cap, u32 rights, u64 offset, u64 len,
                   int reason, utsm_capability *out_cap);

#endif
