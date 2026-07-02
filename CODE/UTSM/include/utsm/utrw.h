#ifndef UTSM_UTRW_H
#define UTSM_UTRW_H

#include <utsm/types.h>
#include <utsm/status.h>
#include <utsm/capability.h>

int utsm_read(utsm_capability cap, u64 offset, void *dst, u64 len);
int utsm_write(utsm_capability cap, u64 offset, const void *src, u64 len);
int utsm_slow_path(int reason);

#endif
