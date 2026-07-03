#include <utsm/net.h>
#include <utsm/log.h>

/*
 * 最小 netdev 注册表：
 * - 早期 DKM 驱动加载路径是单线程顺序执行，当前无需引入锁，避免在网络底座阶段扩大复杂度。
 * - 后续进入多核/热插拔后，应在 register/device_info 外围加自旋锁或 RCU 风格只读快照。
 *
 * 无线扫描接口（scan_start/scan_count/scan_result/is_wireless）通过 dispatch 转发到驱动
 * 注册的回调。有线驱动（e1000/virtio_net）不填这些回调，dispatch 时返回 -3 表示不支持。
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

static int net_scan_start_impl(u32 index) {
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (!dev->scan_start) return -3;
    return dev->scan_start(dev->ctx);
}

static int net_scan_count_impl(u32 index) {
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (!dev->scan_count) return -3;
    return dev->scan_count(dev->ctx);
}

static int net_scan_result_impl(u32 index, u32 n, dkm_net_scan_result *out) {
    if (!out) return -1;
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (!dev->scan_result) return -3;
    return dev->scan_result(dev->ctx, n, out);
}

static int net_is_wireless_impl(u32 index) {
    if (index >= g_net_device_count) return -2;
    const dkm_net_device_desc *dev = &g_net_devices[index];
    if (dev->is_wireless) return dev->is_wireless(dev->ctx);
    return (dev->flags & DKM_NET_F_WIRELESS) ? 1 : 0;
}

static const dkm_net_api g_net_api = {
    .register_device = net_register_device_impl,
    .device_count = net_device_count_impl,
    .device_info = net_device_info_impl,
    .tx = net_tx_impl,
    .rx_poll = net_rx_poll_impl,
    .scan_start = net_scan_start_impl,
    .scan_count = net_scan_count_impl,
    .scan_result = net_scan_result_impl,
    .is_wireless = net_is_wireless_impl
};

const dkm_net_api *net_get_api(void) {
    return &g_net_api;
}
