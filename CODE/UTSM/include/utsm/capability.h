#ifndef UTSM_CAPABILITY_H
#define UTSM_CAPABILITY_H

#include <utsm/types.h>
#include <utsm/status.h>

#define UTSM_RIGHT_READ  (1U << 0)
#define UTSM_RIGHT_WRITE (1U << 1)
#define UTSM_RIGHT_EXEC  (1U << 2)
#define UTSM_RIGHT_SHARE (1U << 3)
#define UTSM_RIGHT_DMA   (1U << 4)

struct utsm_segment_desc;

typedef struct {
    u32 segment_slot;
    u32 generation;
    u32 rights;
    u32 epoch;
    u64 uuid_digest;
    u64 auth_tag;
} utsm_capability;

int utsm_cap_validate(utsm_capability cap, u32 required_rights, u64 offset, u64 len, struct utsm_segment_desc **out_desc);

#endif
