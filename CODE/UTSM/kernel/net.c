#include <utsm/net.h>
#include <utsm/log.h>

/*
 * 最小 netdev 注册表：
 * - 早期 DKM 驱动加载路径是单线程顺序执行，当前无需引入锁，避免在网络底座阶段扩大复杂度。
 * - 后续进入多核/热插拔后，应在 register/device_info 外围加自旋锁或 RCU 风格只读快照。
 */
static dkm_net_device_desc g_net_devices[DKM_NET_MAX_DEVICES];
static u32 g_net_device_count;

void net_init(void) {
    g_net_device_count = 0;
    log_info("[NET] registry init");
}

static int net_register_device_impl(const dkm_net_device_desc *desc) {
    if (!desc || !desc->name) return -1;
    if (g_net_device_count >= DKM_NET_MAX_DEVICES) return -2;

    u32 index = g_net_device_count++;
    g_net_devices[index] = *desc;

    log_info("[NET] device registered");
    log_info(desc->name);
    log_hex64("[NET] index=", index);
    log_hex64("[NET] flags=", desc->flags);
    return (int)index;
}

static u32 net_device_count_impl(void) {
    return g_net_device_count;
}

static int net_device_info_impl(u32 index, dkm_net_device_info *out) {
    if (!out) return -1;
    if (index >= g_net_device_count) return -2;

    const dkm_net_device_desc *dev = &g_net_devices[index];
    out->name = dev->name;
    for (u32 i = 0; i < 6; i++) {
        out->mac[i] = dev->mac[i];
    }
    out->flags = dev->flags;
    return 0;
}

static int net_tx_impl(u32 index, const void *packet, u32 length) {
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (!dev->tx) return -3;
    log_hex64("[NET] tx dispatch fn=", (u64)dev->tx);
    return dev->tx(dev->ctx, packet, length);
}

static int net_rx_poll_impl(u32 index, void *buffer, u32 capacity, u32 *out_length) {
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (!dev->rx_poll) return -3;
    return dev->rx_poll(dev->ctx, buffer, capacity, out_length);
}

static const dkm_net_api g_net_api = {
    .register_device = net_register_device_impl,
    .device_count = net_device_count_impl,
    .device_info = net_device_info_impl,
    .tx = net_tx_impl,
    .rx_poll = net_rx_poll_impl
};

const dkm_net_api *net_get_api(void) {
    return &g_net_api;
}
