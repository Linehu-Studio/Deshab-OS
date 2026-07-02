#include <utsm/block.h>
#include <utsm/log.h>

static dkm_block_device_desc g_devices[DKM_BLOCK_MAX_DEVICES];
static u32 g_device_count;

void block_init(void) {
    g_device_count = 0;
    log_info("[BLOCK] registry init");
}

static int block_register_device_impl(const dkm_block_device_desc *desc) {
    if (!desc || !desc->read || !desc->sector_size) return -1;
    if (g_device_count >= DKM_BLOCK_MAX_DEVICES) return -2;

    u32 index = g_device_count++;
    g_devices[index] = *desc;

    log_info("[BLOCK] device registered");
    if (desc->name) log_info(desc->name);
    log_hex64("[BLOCK] index=", index);
    log_hex64("[BLOCK] sector_size=", desc->sector_size);
    log_hex64("[BLOCK] sector_count=", desc->sector_count);
    return (int)index;
}

static u32 block_device_count_impl(void) {
    return g_device_count;
}

static int block_read_impl(u32 index, u64 lba, u32 count, void *buffer) {
    if (index >= g_device_count || !buffer || count == 0) return -1;
    dkm_block_device_desc *dev = &g_devices[index];
    if (!dev->read) return -2;
    return dev->read(dev->ctx, lba, count, buffer);
}

static u64 block_sector_size_impl(u32 index) {
    if (index >= g_device_count) return 0;
    return g_devices[index].sector_size;
}

static const char *block_device_name_impl(u32 index) {
    if (index >= g_device_count) return 0;
    return g_devices[index].name;
}

static const dkm_block_api g_block_api = {
    .register_device = block_register_device_impl,
    .device_count = block_device_count_impl,
    .read = block_read_impl,
    .sector_size = block_sector_size_impl,
    .device_name = block_device_name_impl
};

const dkm_block_api *block_get_api(void) {
    return &g_block_api;
}
