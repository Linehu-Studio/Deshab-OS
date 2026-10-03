/* virtio_net.c — UTSM virtio-net 设备模拟后端。
 *
 * 为 Linux guest 提供 virtio-net 网卡。guest 的收发包请求经 virtqueue
 * 传入，本后端桥接到 UTSM 真实网卡（net API，e1000/virtio_net DKM 驱动）。
 *
 * Virtio Spec 1.1 §5.1 (Network Device)。
 * 队列：0=RX（device→driver 收包），1=TX（driver→device 发包）。
 * 每帧前置 virtio_net_hdr（10 字节，无 GSO/MRG_RXBUF 协商时全零）。
 *
 * 最小可用实现：
 * - 无 GSO/TSO/checksum offload（feature 不声明，guest 走软件路径）
 * - RX：guest 提交空 buffer，queue_notify / 周期 poll 时调 host rx_poll 填充
 * - TX：queue_notify 时拼接描述符链成帧，直接 net->tx 发出
 */

#include <utsm/virtio_mmio.h>
#include <utsm/panic.h>
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

/* feature bits（除 MAC/STATUS 外都不声明，保持最简） */
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

/* ===== 数据路径统计（低频串口摘要，便于现场诊断 guest 网络问题） ===== */
static u64 g_stat_rx_filled;     /* 已填充给 guest 的 RX 帧数 */
static u64 g_stat_rx_drop_nobuf; /* host 有包但 guest 无可用 RX buffer → 主动丢弃 */
static u64 g_stat_rx_badbuf;     /* guest RX 描述符异常（未映射/hdr 太小/无 data 段） */
static u64 g_stat_tx_sent;       /* 成功送交 host 网卡的 TX 帧数 */
static u64 g_stat_tx_drop;       /* TX 丢弃帧数（链异常/超 MTU/host 发送失败） */

/* 每 512 次数据路径事件打印一行摘要（避免刷串口）。 */
static void net_stat_tick(void) {
    u64 sum = g_stat_rx_filled + g_stat_rx_drop_nobuf + g_stat_rx_badbuf +
              g_stat_tx_sent + g_stat_tx_drop;
    if ((sum & 0x1FF) != 0) return;
    log_hex64("[VNET] stat rx_filled=", g_stat_rx_filled);
    log_hex64("[VNET] stat rx_drop_nobuf=", g_stat_rx_drop_nobuf);
    log_hex64("[VNET] stat rx_badbuf=", g_stat_rx_badbuf);
    log_hex64("[VNET] stat tx_sent=", g_stat_tx_sent);
    log_hex64("[VNET] stat tx_drop=", g_stat_tx_drop);
}

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

/* 从 TX 描述符链拼接一个连续帧到 out_buf（out_cap 容量），返回帧长度。
 * 第一个描述符前置 virtio_net_hdr：Linux 驱动把 hdr 作为【独立的 10 字节
 * 描述符】（len 恰好等于 VIRTIO_NET_HDR_LEN），必须无条件跳过首描述符的
 * 前 min(len,10) 字节，否则 hdr 会被拼进帧首导致 host 发出坏包。
 * 任一段放不下 out_cap（超 MTU 截断）或描述符 GPA 未映射时返回 0（整帧丢弃）。 */
static u32 net_gather_frame(struct virtq_desc *desc, u16 head,
                            u8 *out_buf, u32 out_cap) {
    u32 total = 0;
    u16 cur = head;
    int hops = 0;
    while (hops < 32) {
        u8 *src = (u8 *)virtio_gpa_to_host(desc[cur].addr);
        u32 len = desc[cur].len;
        if (!src) return 0;

        /* 跳过首描述符中的 virtio_net_hdr（hdr 与数据同段时也正确） */
        if (hops == 0) {
            u32 skip = len < VIRTIO_NET_HDR_LEN ? len : VIRTIO_NET_HDR_LEN;
            src += skip;
            len -= skip;
        }
        if (len > 0) {
            if (total + len > out_cap) return 0;
            for (u32 i = 0; i < len; i++) out_buf[total + i] = src[i];
            total += len;
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
        u32 flen = 0;
        if (head < qnum) {
            flen = net_gather_frame(desc, head, g_tx_scratch, sizeof(g_tx_scratch));
        }
        if (flen > 0 && net && g_net_bound >= 0) {
            if (net->tx((u32)g_net_bound, g_tx_scratch, flen) == 0) {
                g_stat_tx_sent++;
            } else {
                /* host ring 满/发送失败：丢帧（guest 不感知 TX 成败） */
                g_stat_tx_drop++;
            }
        } else {
            /* 链异常/超 MTU 截断保护/未绑定网卡：丢帧 */
            g_stat_tx_drop++;
        }
        /* TX buffer 必须回执（guest 回收 skb），len 字段对 TX 无意义 */
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = 0;
        used_idx++;
        cur++;
        net_stat_tick();
    }
    virtio_queue_bump_used(used, used_idx);
    g_tx_last_avail = cur;
}

/* RX queue（idx=0）：guest 提交空 buffer，我们调 host rx_poll 填充。
 *
 * buffer 所有权规则（关键，避免泄漏）：
 *   - guest 一次性投满 RX 队列后只 notify 一次，之后等 used 事件。
 *   - 有包：填充 hdr+data → used ring 回执 → guest 收包后重投 buffer。
 *   - 无包：【必须停止且不消耗该 buffer】（g_rx_last_avail 保持指向它），
 *     由 virtio_net_poll() 在 preemption timer 路径周期性重试；
 *     否则 buffer 被跳过且永远不进 used ring → guest RX buffer 耗尽饿死。
 *   - guest 无可用 buffer 时：主动把 host 收到的包收出来丢弃并统计，
 *     避免 e1000 8 深 RX ring 堵死后网卡级无差别丢包（virtio 语义上
 *     device 在 RX 无 buffer 时丢包是标准行为）。
 * 填充 ≥1 个 buffer 后由调用方负责注入 guest IRQ。 */
static u16 g_rx_last_avail;
static u8 g_rx_drop_scratch[2048];  /* 无 guest buffer 时的丢弃暂存 */

/* 解析 RX 描述符链的 data 接收区（跳过首描述符的 hdr 区）。
 * 支持两种布局：
 *   A. hdr(10B, W) + data(N, W) 两个描述符（Linux add_recvbuf_small 布局）
 *   B. hdr+data 同在一个描述符（len > 10，data 从偏移 10 起）
 * 成功返回 0 并填充 datap/datacap/hdrp 输出参数；失败返回 -1（链异常）。 */
static int net_rx_data_region(struct virtq_desc *desc, u16 head,
                              u8 **datap, u32 *datacap, u8 **hdrp) {
    struct virtq_desc *d0 = &desc[head];
    u8 *hdr = (u8 *)virtio_gpa_to_host(d0->addr);
    if (!hdr || d0->len < VIRTIO_NET_HDR_LEN) return -1;
    *hdrp = hdr;

    if (d0->len > VIRTIO_NET_HDR_LEN) {
        /* 布局 B：hdr 与 data 同段 */
        if (!(d0->flags & VIRTQ_DESC_F_WRITE)) return -1;
        *datap = hdr + VIRTIO_NET_HDR_LEN;
        *datacap = d0->len - VIRTIO_NET_HDR_LEN;
        return 0;
    }
    /* 布局 A：hdr 独立描述符，data 在 NEXT 段 */
    if (!(d0->flags & VIRTQ_DESC_F_NEXT)) return -1;
    struct virtq_desc *d1 = &desc[d0->next];
    if (!(d1->flags & VIRTQ_DESC_F_WRITE)) return -1;
    u8 *data = (u8 *)virtio_gpa_to_host(d1->addr);
    if (!data || d1->len == 0) return -1;
    *datap = data;
    *datacap = d1->len;
    return 0;
}

/* 处理 RX 队列。返回本次填充的 buffer 数（0 = 无包/无可填）。 */
static int net_handle_rx(void) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs(VIRTIO_ID_NET, 0, &desc, &avail, &used, &qnum) != 0)
        return 0;

    const dkm_net_api *net = net_get_api();
    if (!net || g_net_bound < 0) return 0;

    int filled = 0;
    u16 cur = g_rx_last_avail;
    u16 used_idx = used->idx;
    int budget = 32;  /* 单次调用处理上限，避免 vmexit/poll 路径停留过久 */

    while (budget-- > 0) {
        if (cur == avail->idx) {
            /* guest 无可用 RX buffer：主动收包丢弃，防 host RX ring 堵死 */
            u32 got = 0;
            if (net->rx_poll((u32)g_net_bound, g_rx_drop_scratch,
                             sizeof(g_rx_drop_scratch), &got) == 0 && got > 0) {
                g_stat_rx_drop_nobuf++;
                net_stat_tick();
                continue;
            }
            break;  /* host 也无包 */
        }

        u16 head = avail->ring[cur % qnum];
        u8 *hdrp = (u8 *)0;
        u8 *datap = (u8 *)0;
        u32 datacap = 0;
        if (head >= qnum ||
            net_rx_data_region(desc, head, &datap, &datacap, &hdrp) != 0) {
            /* 异常 buffer：len=0 回执让 guest 回收重投，避免卡死 */
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = 0;
            used_idx++;
            cur++;
            filled++;
            g_stat_rx_badbuf++;
            net_stat_tick();
            continue;
        }

        u32 got = 0;
        if (net->rx_poll((u32)g_net_bound, datap, datacap, &got) == 0 && got > 0) {
            /* 填 virtio_net_hdr 全零（无 GSO） */
            for (int i = 0; i < VIRTIO_NET_HDR_LEN; i++) hdrp[i] = 0;
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = VIRTIO_NET_HDR_LEN + got;
            used_idx++;
            cur++;
            filled++;
            g_stat_rx_filled++;
            net_stat_tick();
        } else {
            /* 无包：停止，不消耗 cur 指向的 buffer，下次 poll 重试 */
            break;
        }
    }
    virtio_queue_bump_used(used, used_idx);
    g_rx_last_avail = cur;
    return filled;
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

/* 周期轮询入口（vmexit preemption timer 路径调用，~1kHz）。
 * guest 投满 RX buffer 后不再 notify，host 侧包到达时靠此轮询填充
 * 并注入 IRQ，guest 才能及时收包（pacman/ping 等依赖）。 */
void virtio_net_poll(void) {
    if (g_net_bound < 0) return;
    if (net_handle_rx() > 0) {
        virtio_mmio_raise_irq(VIRTIO_ID_NET);
    }
}
