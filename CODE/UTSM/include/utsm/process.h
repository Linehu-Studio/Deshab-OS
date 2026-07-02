#ifndef UTSM_PROCESS_H
#define UTSM_PROCESS_H

#include <utsm/types.h>
#include <utsm/uuid.h>

typedef struct utsm_process_context {
    uuid128_t process_uuid;
    u64 crypto_epoch;
    u64 recovery_generation;
    u32 capability_table_slot;
    u32 capability_count;
    u32 flags;
} utsm_process_context;

void utsm_process_create_test(utsm_process_context *process, u64 seed);

#endif
