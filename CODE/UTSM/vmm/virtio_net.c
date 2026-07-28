/* virtio_net.c — UTSM virtio-net 设备模拟后端。
 *
 * 为 Linux guest 提供 virtio-net 网卡。guest 的收发包请求经 virtqueue
 * 传入，本后端桥接到 UTSM 真实网卡（net API，e1000/virtio_net DKM 驱动）。
 *
 * Virtio Spec 1.1 §5.1 (Network Device)。
 * 队列：0=RX（device→driver 收包），1=TX（driver→device 发包）。
 * 每帧前置 virtio_net_hdr（10/12 字节，无 GSO 时全零）。
 *
 * 最小可用实现：
 * - 无 GSO/TSO/checksum offload（feature 不声明，guest 走软件路径）
 * - RX 轮询：guest RX 队列提交空 buffer，queue_notify 时尝试 rx_poll
 * - TX：queue_notify 时取帧直接 net->tx 发出
 */

#include <utsm/virtio_mmio.h>
#include <utsm/linux_loader.h>
#include <utsm/net.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* virtio_net_hdr（VIRTIO_NET_F_CSUM/GSO 未协商时为 10 字节 legacy） */
struct virtio_net_hdr {
    u8 flags;
    u8 gso_type;
    u16 hdr_len;
    u16 gso_size;
    u16 csum_start;
    u16 csum_offset;
    /* u16 num_buffers;  // 仅 VIRTIO_NET_F_MRG_RXBUF 协商后存在 */
} __attribute__((packed));

#define VIRTIO_NET_HDR_LEN 10

/* feature bits（我们都不声明，保持最简） */
#define VIRTIO_NET_F_CSUM       0
#define VIRTIO_NET_F_GUEST_CSUM 1
#define VIRTIO_NET_F_MAC        5
#define VIRTIO_NET_F_STATUS     16

/* device config（VIRTIO_MMIO_CONFIG 起） */
struct virtio_net_config {
    u8 mac[6];
    u16 status;         /* bit0 = link up */
    u16 max_virtqueue_pairs;
} __attribute__((packed));

static u8 g_net_mac[6] = { 0x52, 0x54, 0x00, 0xDE, 0x5A, 0x01 };
static int g_net_bound = -1;

static u32 net_read_config(u32 offset, int width) {
    (void)width;
    struct virtio_net_config cfg;
    u8 *c = (u8 *)&cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) c[i] = 0;
    for (int i = 0; i < 6; i++) cfg.mac[i] = g_net_mac[i];
    cfg.status = 1;             /* link up */
    cfg.max_virtqueue_pairs = 1;

    if (offset + 4 <= sizeof(cfg)) {
        u8 *src = (u8 *)&cfg + offset;
        return (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
    }
    return 0;
}

/* 从描述符链拼接一个连续帧（virtio-net 帧通常 2 个描述符：hdr + data）。
 * 返回帧长度，数据拷入 out_buf（out_cap 容量）。 */
static u32 net_gather_frame(struct virtq_desc *desc, u16 head,
                            u8 *out_buf, u32 out_cap, int skip_hdr) {
    u32 total = 0;
    u16 cur = head;
    int hops = 0;
    while (hops < 32) {
        u8 *src = (u8 *)virtio_gpa_to_host(desc[cur].addr);
        u32 len = desc[cur].len;
        if (!src) break;

        /* 跳过第一个描述符中的 virtio_net_hdr */
        if (hops == 0 && skip_hdr && len > VIRTIO_NET_HDR_LEN) {
            u32 dl = len - VIRTIO_NET_HDR_LEN;
            if (total + dl <= out_cap) {
                u8 *d = src + VIRTIO_NET_HDR_LEN;
                for (u32 i = 0; i < dl; i++) out_buf[total + i] = d[i];
                total += dl;
            }
        } else {
            if (total + len <= out_cap) {
                for (u32 i = 0; i < len; i++) out_buf[total + i] = src[i];
                total += len;
            }
        }

        if (!(desc[cur].flags & VIRTQ_DESC_F_NEXT)) break;
        cur = desc[cur].next;
        hops++;
    }
    return total;
}

/* TX queue（idx=1）：guest 发包。 */
static u16 g_tx_last_avail;
static u8 g_tx_scratch[2048];

static void net_handle_tx(void) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs(VIRTIO_ID_NET, 1, &desc, &avail, &used, &qnum) != 0)
        return;

    const dkm_net_api *net = net_get_api();
    u16 cur = g_tx_last_avail;
    u16 used_idx = used->idx;
    while (cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        u32 flen = net_gather_frame(desc, head, g_tx_scratch, sizeof(g_tx_scratch), 1);
        if (flen > 0 && net && g_net_bound >= 0) {
            net->tx((u32)g_net_bound, g_tx_scratch, flen);
        }
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = 0;
        used_idx++;
        cur++;
    }
    virtio_queue_bump_used(used, used_idx);
    g_tx_last_avail = cur;
}

/* RX queue（idx=0）：guest 提交空 buffer，我们尝试 rx_poll 填充。
 * guest 每提交一批 buffer 就 notify 一次；有包则填充 + used，无包则
 * 保留 buffer 等下次 notify（简化：直接返回 0 长度 used，让 guest 重投）。 */
static u16 g_rx_last_avail;

static void net_handle_rx(void) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs(VIRTIO_ID_NET, 0, &desc, &avail, &used, &qnum) != 0)
        return;

    const dkm_net_api *net = net_get_api();
    if (!net || g_net_bound < 0) return;

    u16 cur = g_rx_last_avail;
    u16 used_idx = used->idx;
    while (cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        /* RX 描述符链：hdr(W) + data(W)。尝试收一包。 */
        u16 curd = head;
        u8 *hdrp = (u8 *)virtio_gpa_to_host(desc[curd].addr);
        u8 *datap = (u8 *)0;
        u32 datalen = 0;
        if (desc[curd].flags & VIRTQ_DESC_F_NEXT) {
            u16 di = desc[curd].next;
            datap = (u8 *)virtio_gpa_to_host(desc[di].addr);
            datalen = desc[di].len;
        }
        if (!hdrp || !datap || datalen == 0) { cur++; continue; }

        u32 got = 0;
        if (net->rx_poll((u32)g_net_bound, datap, datalen, &got) == 0 && got > 0) {
            /* 填 virtio_net_hdr 全零（无 GSO） */
            for (int i = 0; i < VIRTIO_NET_HDR_LEN; i++) hdrp[i] = 0;
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = VIRTIO_NET_HDR_LEN + got;
            used_idx++;
        } else {
            /* 无包：不消耗该 buffer，留待下次（但为避免死循环，跳过） */
        }
        cur++;
    }
    virtio_queue_bump_used(used, used_idx);
    g_rx_last_avail = cur;
}

static void net_queue_notify(u32 queue_idx) {
    if (queue_idx == 1) net_handle_tx();
    else if (queue_idx == 0) net_handle_rx();
}

static void net_reset(void) {
    g_tx_last_avail = 0;
    g_rx_last_avail = 0;
}

static struct virtio_backend g_net_backend = {
    .device_id = VIRTIO_ID_NET,
    .gpa_base = VIRTIO_MMIO_NET_GPA,
    .irq = 6,                   /* guest ISA IRQ6（cmdline :6，vector 0x36） */
    .num_queues = 2,
    .queue_size = 128,
    .device_features = (1ULL << VIRTIO_F_VERSION_1) |
                       (1ULL << VIRTIO_NET_F_MAC) | (1ULL << VIRTIO_NET_F_STATUS),
    .config_len = sizeof(struct virtio_net_config),
    .read_config = net_read_config,
    .queue_notify = net_queue_notify,
    .reset = net_reset,
};

void virtio_net_backend_init(void) {
    /* 绑定 UTSM 第 0 个网卡（e1000 或 virtio_net DKM 驱动） */
    const dkm_net_api *net = net_get_api();
    if (net && net->device_count() > 0) {
        g_net_bound = 0;
        dkm_net_device_info info;
        if (net->device_info(0, &info) == 0) {
            for (int i = 0; i < 6; i++) g_net_mac[i] = info.mac[i];
        }
        log_info("[VNET] bound to UTSM net device 0");
    } else {
        g_net_bound = -1;
        log_warn("[VNET] no UTSM net device, net backend degraded");
    }
    virtio_mmio_register(&g_net_backend);
}
