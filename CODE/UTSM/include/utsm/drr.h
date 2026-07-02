#ifndef UTSM_DRR_H
#define UTSM_DRR_H

#include <utsm/types.h>

void drr_stub_init(void);
const u64 *drr_get_root_key(void);
u64 drr_recovery_generation(void);
void drr_report_fault(const char *reason);
void drr_log_write_intent(u32 segment_slot, u64 offset, u64 len);
void drr_log_write_commit(u32 segment_slot, u64 offset, u64 len);

#endif
