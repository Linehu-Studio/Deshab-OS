#include <utsm/utsm.h>
#include <utsm/log.h>
#include <utsm/segment.h>

int memcmp(const void *a, const void *b, usize len);
usize strlen(const char *s);

int utsm_selftest_run(void) {
    utsm_process_context process;
    utsm_process_create_test(&process, 1);

    utsm_capability cap;
    int status = utsm_create_segment(&process, UTSM_PAGE_SIZE, UTSM_SEG_F_READ | UTSM_SEG_F_WRITE | UTSM_SEG_F_STRONG_RECOVERY, &cap);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest create segment failed");
        return status;
    }

    const char *message = "deshab utsm sealed memory selftest";
    u64 len = strlen(message) + 1;
    status = utsm_write(cap, 128, message, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest write failed");
        return status;
    }
    log_info("[UTSM] selftest write ok");

    utsm_segment_desc *desc = utsm_get_segment(cap.segment_slot);
    if (!desc) {
        return UTSM_ERR_INVALID;
    }
    if (memcmp(desc->cipher_base + 128, message, len) == 0) {
        log_error("[UTSM] raw cipher equals plaintext");
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] raw cipher check ok");

    char readback[64];
    status = utsm_read(cap, 128, readback, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest read failed");
        return status;
    }
    if (memcmp(readback, message, len) != 0) {
        log_error("[UTSM] selftest read mismatch");
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] selftest read ok");
    return UTSM_OK;
}
