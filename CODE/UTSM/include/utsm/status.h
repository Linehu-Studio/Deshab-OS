#ifndef UTSM_STATUS_H
#define UTSM_STATUS_H

typedef enum {
    UTSM_OK = 0,
    UTSM_ERR_INVALID = -1,
    UTSM_ERR_NO_MEMORY = -2,
    UTSM_ERR_ACCESS = -3,
    UTSM_ERR_BOUNDS = -4,
    UTSM_ERR_STALE_CAP = -5,
    UTSM_ERR_EPOCH = -6,
    UTSM_ERR_KEY_MISS = -7,
    UTSM_ERR_STATE = -8,
    UTSM_ERR_POISONED = -9,
    UTSM_ERR_RETRY = -10,
    UTSM_ERR_MAC_FAILED = -11   /* U2: 页 MAC 校验失败（slow path 处置） */
} utsm_status;

#endif
