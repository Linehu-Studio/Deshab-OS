#ifndef UTSM_NET_H
#define UTSM_NET_H

#include <utsm/types.h>

#define DKM_NET_MAX_DEVICES 8u

/* 网卡链路/能力标志。 */
#define DKM_NET_F_LINK_UP  (1u << 0)
#define DKM_NET_F_TX_READY (1u << 1)
#define DKM_NET_F_RX_READY (1u << 2)

typedef int (*dkm_net_tx_fn)(void *ctx, const void *packet, u32 length);
typedef int (*dkm_net_rx_poll_fn)(void *ctx, void *buffer, u32 capacity, u32 *out_length);

typedef struct dkm_net_device_desc {
    const char *name;
    u8 mac[6];
    u32 flags;
    void *ctx;
    dkm_net_tx_fn tx;
    dkm_net_rx_poll_fn rx_poll;
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
} dkm_net_api;

void net_init(void);
const dkm_net_api *net_get_api(void);

#endif
