#include <utsm/dkm.h>

static const char *const console_early_provides[] = {
    "console",
    "early-log"
};

static const dkm_driver_desc console_early_desc = {
    .magic = DKM_DRIVER_MAGIC,
    .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(dkm_driver_desc),
    .name = "console_early",
    .version = "0.1.0",
    .vendor = "Deshab",
    .driver_class = DKM_CLASS_CONSOLE,
    .stage = 0,
    .flags = DKM_F_REQUIRED | DKM_F_BOOT_MODULE | DKM_F_NO_UNLOAD | DKM_F_EARLY_LOG,
    .priority = 0,
    .depends = 0,
    .depends_count = 0,
    .provides = console_early_provides,
    .provides_count = 2,
    .min_kernel_abi = DKM_KERNEL_API_VERSION,
    .feature_bits = 0,
    .reserved0 = 0,
    .reserved1 = 0
};

static int console_early_init(const dkm_kernel_api *api, dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log || !api->log->info) {
        return -1;
    }
    api->log->info("[DKM:console_early] serial console online");
    return 0;
}

static int console_early_exit(dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}

const dkm_builtin_driver dkm_builtin_console_early = {
    .desc = &console_early_desc,
    .init = console_early_init,
    .exit = console_early_exit
};
