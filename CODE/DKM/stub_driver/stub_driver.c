#include <stdint.h>

#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1

#ifndef DRIVER_NAME
#define DRIVER_NAME stub
#endif

#define DKM_STRINGIFY_INNER(x) #x
#define DKM_STRINGIFY(x) DKM_STRINGIFY_INNER(x)
#define DRIVER_NAME_STRING DKM_STRINGIFY(DRIVER_NAME)

#ifndef DRIVER_CLASS
#define DRIVER_CLASS 0
#endif

#ifndef DRIVER_STAGE
#define DRIVER_STAGE 0
#endif

#ifndef DRIVER_FLAGS
#define DRIVER_FLAGS 0
#endif

#ifndef DRIVER_PRIORITY
#define DRIVER_PRIORITY 0
#endif

struct dkm_driver_desc {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t desc_size;

    const char *name;
    const char *version;
    const char *vendor;

    uint32_t driver_class;
    uint32_t stage;
    uint32_t flags;
    uint32_t priority;

    const char *const *depends;
    uint32_t depends_count;

    const char *const *provides;
    uint32_t provides_count;

    uint64_t min_kernel_abi;
    uint64_t feature_bits;

    uint64_t reserved0;
    uint64_t reserved1;
};

struct dkm_kernel_api;
struct dkm_driver_handle;

__attribute__((visibility("default")))
const struct dkm_driver_desc driver_desc = {
    .magic = DKM_DRIVER_MAGIC,
    .abi_version = DKM_ABI_VERSION,
    .desc_size = sizeof(struct dkm_driver_desc),
    .name = DRIVER_NAME_STRING,
    .version = "0.1.0-stub",
    .vendor = "Deshab",
    .driver_class = DRIVER_CLASS,
    .stage = DRIVER_STAGE,
    .flags = DRIVER_FLAGS,
    .priority = DRIVER_PRIORITY,
    .depends = 0,
    .depends_count = 0,
    .provides = 0,
    .provides_count = 0,
    .min_kernel_abi = 1,
    .feature_bits = 0,
    .reserved0 = 0,
    .reserved1 = 0
};

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api, struct dkm_driver_handle *handle) {
    (void)api;
    (void)handle;
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
