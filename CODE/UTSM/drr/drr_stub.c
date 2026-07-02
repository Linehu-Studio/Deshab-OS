#include <utsm/drr.h>
#include <utsm/log.h>

static const u64 g_root_key[4] = {
    0x4452525554534d31ULL,
    0xfeedfacecafebeefULL,
    0x0123456789abcdefULL,
    0xfedcba9876543210ULL
};

void drr_stub_init(void) {
}

const u64 *drr_get_root_key(void) {
    return g_root_key;
}

u64 drr_recovery_generation(void) {
    return 1;
}

void drr_report_fault(const char *reason) {
    log_error("[DRR] fault reported");
    log_error(reason);
}

void drr_log_write_intent(u32 segment_slot, u64 offset, u64 len) {
    UTSM_UNUSED(segment_slot);
    UTSM_UNUSED(offset);
    UTSM_UNUSED(len);
}

void drr_log_write_commit(u32 segment_slot, u64 offset, u64 len) {
    UTSM_UNUSED(segment_slot);
    UTSM_UNUSED(offset);
    UTSM_UNUSED(len);
}
