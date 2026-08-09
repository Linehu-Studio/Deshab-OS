/* hello-driver - Deshab SDK 最小驱动示例
 *
 * 展示 DKM 驱动三符号契约：driver_desc + driver_init + driver_exit。
 * 构建方式：clang -c main.c -o hello.o && ld.lld -r -o hello.drv hello.o
 * 注册方式：在 SYSTEM/driver/manifest.json 添加条目
 */
#include "deshab/driver.h"
#include "deshab/serial.h"

static const dkm_log_api *g_log = 0;

static const char *const g_depends[] = { 0 };
static const char *const g_provides[] = { "hello" };

__attribute__((visibility("default")))
const dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(dkm_driver_desc),
    .name           = "hello",
    .version        = "1.0",
    .vendor         = "Deshab",
    .driver_class   = DKM_CLASS_MISC,
    .stage          = 3,
    .flags          = 0,
    .priority       = 100,
    .depends        = g_depends,
    .depends_count  = 0,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = DKM_KERNEL_API_VERSION,
    .feature_bits   = 0,
};

__attribute__((visibility("default")))
int driver_init(const dkm_kernel_api *api, dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = api->log;
    g_log->info("[hello] init");
    g_log->info("[hello] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(dkm_driver_handle *handle) {
    (void)handle;
    if (g_log) g_log->info("[hello] exit");
    return 0;
}
