#include <utsm/dkm.h>
#include <utsm/log.h>

static int dkm_validate_desc(const dkm_driver_desc *desc) {
    if (!desc) {
        return -1;
    }
    if (desc->magic != DKM_DRIVER_MAGIC) {
        return -2;
    }
    if (desc->abi_version != DKM_ABI_VERSION) {
        return -3;
    }
    if (desc->desc_size < sizeof(dkm_driver_desc)) {
        return -4;
    }
    if (!desc->name || !desc->version) {
        return -5;
    }
    return 0;
}

void dkm_init(void) {
    log_info("[DKM] init begin");
    int status = dkm_load_builtin(&dkm_builtin_console_early);
    if (status == 0) {
        log_info("[DKM] init ok");
    } else {
        log_error("[DKM] init failed");
    }
}

int dkm_load_builtin(const dkm_builtin_driver *driver) {
    if (!driver || !driver->desc || !driver->init) {
        log_error("[DKM] invalid builtin driver");
        return -1;
    }

    int status = dkm_validate_desc(driver->desc);
    if (status != 0) {
        log_error("[DKM] driver_desc rejected");
        return status;
    }

    dkm_driver_handle handle;
    handle.desc = driver->desc;
    handle.state = DKM_STATE_ABI_CHECKED;
    handle.init_status = 0;
    handle.load_stage = driver->desc->stage;
    handle.flags = driver->desc->flags;

    log_info("[DKM] builtin driver init begin");
    log_info(driver->desc->name);
    handle.state = DKM_STATE_INITING;
    status = driver->init(dkm_get_kernel_api(), &handle);
    handle.init_status = status;

    if (status != 0) {
        handle.state = DKM_STATE_FAILED;
        log_error("[DKM] builtin driver init failed");
        return status;
    }

    handle.state = DKM_STATE_ACTIVE;
    log_info("[DKM] builtin driver active");
    return 0;
}
