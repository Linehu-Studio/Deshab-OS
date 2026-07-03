#ifndef UTSM_NET_H
#define UTSM_NET_H

#include <utsm/types.h>

#define DKM_NET_MAX_DEVICES 8u
#define DKM_NET_MAX_SCAN_RESULTS 32u
#define DKM_NET_SSID_MAX 32u

/* 网卡链路/能力标志。 */
#define DKM_NET_F_LINK_UP  (1u << 0)
#define DKM_NET_F_TX_READY (1u << 1)
#define DKM_NET_F_RX_READY (1u << 2)
#define DKM_NET_F_WIRELESS (1u << 3)

/* WiFi 扫描结果加密类型。 */
#define DKM_NET_SEC_OPEN   0u
#define DKM_NET_SEC_WEP    1u
#define DKM_NET_SEC_WPA    2u
#define DKM_NET_SEC_WPA2   3u
#define DKM_NET_SEC_WPA3   4u

typedef int (*dkm_net_tx_fn)(void *ctx, const void *packet, u32 length);
typedef int (*dkm_net_rx_poll_fn)(void *ctx, void *buffer, u32 capacity, u32 *out_length);

/* WiFi 扫描结果（须先于回调 typedef 定义）。 */
typedef struct dkm_net_scan_result {
    char ssid[DKM_NET_SSID_MAX + 1];
    u8 bssid[6];
    u8 channel;
    i8 rssi;       /* dBm，负值 */
    u8 security;   /* DKM_NET_SEC_* */
} dkm_net_scan_result;

/* 无线扫描回调（无线驱动填，有线驱动置 NULL）。 */
typedef int (*dkm_net_scan_start_fn)(void *ctx);
typedef int (*dkm_net_scan_count_fn)(void *ctx);
typedef int (*dkm_net_scan_result_fn)(void *ctx, u32 n, dkm_net_scan_result *out);
typedef int (*dkm_net_is_wireless_fn)(void *ctx);

typedef struct dkm_net_device_desc {
    const char *name;
    u8 mac[6];
    u32 flags;
    void *ctx;
    dkm_net_tx_fn tx;
    dkm_net_rx_poll_fn rx_poll;
    /* 无线扩展回调（无线驱动填，有线驱动置 NULL，向后兼容）。 */
    dkm_net_scan_start_fn   scan_start;
    dkm_net_scan_count_fn   scan_count;
    dkm_net_scan_result_fn  scan_result;
    dkm_net_is_wireless_fn  is_wireless;
} dkm_net_device_desc;

typedef struct dkm_net_device_info {
    const char *name;
    u8 mac[6];
    u32 flags;
} dkm_net_device_info;

typedef struct dkm_net_api {
    int (*register_device)(const dkm_net_device_desc *desc);
    u32 (*device_count)(void);
    int (*device_info)(u32 index, dkm_net_device_info *out);
    int (*tx)(u32 index, const void *packet, u32 length);
    int (*rx_poll)(u32 index, void *buffer, u32 capacity, u32 *out_length);
    /* 无线扫描接口（dispatch 到驱动回调，有线设备返回 -3 表示不支持）。 */
    int (*scan_start)(u32 index);
    int (*scan_count)(u32 index);
    int (*scan_result)(u32 index, u32 n, dkm_net_scan_result *out);
    int (*is_wireless)(u32 index);
} dkm_net_api;

void net_init(void);
const dkm_net_api *net_get_api(void);

#endif
