/* DKM virtio-net Driver — virtio network device (legacy/transitional PCI)
 * Stage 3 optional network driver, depends on "pci" and "irq", provides "netdev".
 *
 * 数据路径实现（virtio spec 1.0 legacy interface，transitional device 1AF4:1001）:
 *   - PCI 探测 → BAR0 legacy I/O 窗口
 *   - reset → ACKNOWLEDGE → DRIVER → feature negotiation（MAC/STATUS 子集）
 *     → FEATURES_OK → virtqueue 建立（RXQ0/TXQ1, legacy split-ring 布局）
 *     → DRIVER_OK
 *   - RX: 预填 32×2048B buffer 到 RXQ avail；rx_poll 收割 used ring，
 *     剥 10B virtio_net_hdr 后把以太网帧交给上层协议栈，buffer 回收重填
 *   - TX: 8×2048B buffer 池（desc 索引与 buffer 1:1 映射），hdr+帧单描述符，
 *     QueueNotify kick，与 e1000 一致的同步等待完成（TSC 超时兜底）
 *   - boot 自测: 经真实 QEMU user-net 后端向 10.0.2.2 DHCP server 发 DISCOVER、
 *     收 OFFER，一次跑通证明 TX/RX 全链路（DMA/notify/used ring）
 *   - IRQ 按 IDT vector（irq_line+0x20）注册做 ISR 统计；PIC 未解屏蔽时
 *     自动退化为纯轮询，不依赖中断正确性
 */

#include "../dkm_shared.h"

/* virtio-specific PCI IDs and capability types (driver-local) */
#define PCI_CLASS_REG  0x08   /* 32-bit class/subclass/prog_if/revision */

#define PCI_VENDOR_VIRTIO       0x1AF4u
/* PCI device ID 约定见 virtio_is_net_device(): transitional net=0x1000, modern net=0x1041 */

#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u
#define VIRTIO_PCI_CAP_PCI_CFG     5u

/* ---- legacy I/O 寄存器偏移（virtio spec 1.0 §4.1.4, BAR0 I/O space） ---- */
#define VP_HOST_FEATURES   0x00u   /* u32 RO: device feature bits 0..31 */
#define VP_GUEST_FEATURES  0x04u   /* u32 WO: driver 接受的 feature 子集 */
#define VP_QUEUE_PFN       0x08u   /* u32 WO: 队列物理地址 >> 12 */
#define VP_QUEUE_NUM       0x0Cu   /* u16 RO: 队列容量（2 的幂） */
#define VP_QUEUE_SEL       0x0Eu   /* u16 WO: 选择操作的队列 */
#define VP_QUEUE_NOTIFY    0x10u   /* u16 WO: kick 指定队列 */
#define VP_STATUS          0x12u   /* u8  RW: device status */
#define VP_ISR             0x13u   /* u8  RO: ISR，读即清除 */
#define VP_NET_CFG_MAC     0x14u   /* 6B  RO: MAC（device config 区起始） */

/* ---- device status bits ---- */
#define VS_ACKNOWLEDGE   0x01u
#define VS_DRIVER        0x02u
#define VS_DRIVER_OK     0x04u
#define VS_FEATURES_OK   0x08u

/* ---- virtio-net feature bits（spec §5.1.6），只取最小子集 ---- */
#define VNET_F_MAC       (1u << 5)    /* device config 提供 MAC */
#define VNET_F_STATUS    (1u << 16)   /* device config 提供 link status */

/* ---- vring ---- */
#define VRING_DESC_F_WRITE  2u        /* descriptor 由 device 写（RX） */
#define VNET_HDR_SIZE       10u       /* 未协商 MRG_RXBUF → virtio_net_hdr 为 10 字节 */

#define RX_BUF_COUNT   32u            /* RX 预填 buffer 数（不必填满整个队列） */
#define TX_BUF_COUNT   8u             /* TX buffer 池大小 */
#define VNET_BUF_SIZE  2048u          /* 10B hdr + 最大 1518B 以太网帧 */

/* split-ring 结构（legacy，x86 上字段为 guest 原生小端序） */
struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct vring_used_elem {
    u32 id;
    u32 len;
};

struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[];
};

struct vring_used {
    u16 flags;
    u16 idx;
    struct vring_used_elem ring[];
};

/* 单个 virtqueue 的运行时视图。
 * legacy 布局（spec §2.4.2）:
 *   base+0                       : descriptor table（16B × size）
 *   base+16*size                 : avail ring（4 + 2×size + 2B used_event）
 *   align4K(16*size + 6 + 2*size): used ring（4 + 8×size + 2B avail_event）
 * 整段必须物理连续且 4KiB 对齐（QueuePFN 只接受页帧号）。 */
struct virtq {
    u16 qsel;                       /* queue select 索引（0=RX, 1=TX） */
    u16 size;                       /* device 报告的队列容量 */
    u64 phys;                       /* 队列内存物理基址（页对齐） */
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    u16 last_used_idx;              /* driver 侧 used ring 收割游标 */
};

static const char *const g_depends[] = { "pci", "irq" };
static const char *const g_provides[] = { "netdev" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "virtio_net",
    .version        = "0.2.0",
    .vendor         = "Deshab",
    .driver_class   = 9,   /* DKM_CLASS_NET */
    .stage          = 3,
    .flags          = 0,
    .priority       = 0,
    .depends        = g_depends,
    .depends_count  = 2,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

static const struct dkm_log_api *g_log;

/* ---- 全局设备状态 ---- */
static u16 g_io_base;                       /* legacy I/O BAR 基址，0=未初始化 */
static struct virtq g_rxq;
static struct virtq g_txq;
static u8  *g_rx_buf[RX_BUF_COUNT];
static u64 g_rx_buf_phys[RX_BUF_COUNT];
static u8  *g_tx_buf[TX_BUF_COUNT];
static u64 g_tx_buf_phys[TX_BUF_COUNT];
static u8  g_tx_free[TX_BUF_COUNT];         /* 1=空闲可提交 */
static int g_ready;                         /* 数据路径就绪标志 */
static u8  g_mac[6];

/* ---- 统计计数器（串口可观测） ---- */
static u64 g_stat_tx_ok;
static u64 g_stat_tx_err;
static u64 g_stat_tx_timeout;
static u64 g_stat_rx_ok;
static u64 g_stat_rx_err;
static u64 g_stat_rx_overflow;
static u64 g_stat_irq_count;

/* ---- 自测缓冲区（静态 bss，避免内核栈压力） ---- */
static u8  g_st_frame[360];                 /* DHCP DISCOVER 帧（342B） */
static u8  g_st_buf[2048];                  /* RX 收割暂存 */

/* ---- 16-bit I/O port 访问（dkm_shared.h 只提供 8/32 位） ---- */
static __inline__ void vn_outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u16 vn_inw(u16 port) {
    u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* device DMA 内存屏障：描述符/ring 写入必须先于 idx 更新对设备可见。
 * x86 store-store 有序，compiler fence + mfence 双保险（控制路径，开销可忽略）。 */
static __inline__ void vn_fence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    return dkm_pci_read(bus, dev, func, reg);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 value) {
    dkm_pci_write(bus, dev, func, reg, value);
}

static u8 pci_read8(u8 bus, u8 dev, u8 func, u8 reg) {
    return dkm_pci_read8(bus, dev, func, reg);
}

static void copy_bytes(void *dst, const void *src, u32 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < len; i++) d[i] = s[i];
}

static void zero_bytes(void *dst, u32 len) {
    u8 *d = (u8 *)dst;
    for (u32 i = 0; i < len; i++) d[i] = 0;
}

static void log_hex(const char *prefix, u64 value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    u32 pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(value >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static u32 dec_u32(u32 v, char *out) {
    char tmp[10];
    u32 n = 0;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v && n < 10) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (u32 i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* 打印 IPv4 地址。ip 为主机序（高字节 = 第一个 octet，如 0x0A00020F = 10.0.2.15）。 */
static void log_ipv4(const char *prefix, u32 ip) {
    char buf[24];
    u32 p = 0;
    for (u32 i = 0; i < 4; i++) {
        u8 octet = (u8)(ip >> ((3 - i) * 8));
        p += dec_u32(octet, buf + p);
        if (i < 3) buf[p++] = '.';
    }
    buf[p] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static void log_mac(const char *prefix, const u8 mac[6]) {
    static const char hex[] = "0123456789abcdef";
    char buf[18];
    u32 p = 0;
    for (u32 i = 0; i < 6; i++) {
        buf[p++] = hex[(mac[i] >> 4) & 0xf];
        buf[p++] = hex[mac[i] & 0xf];
        if (i < 5) buf[p++] = ':';
    }
    buf[p] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

/* virtio-net PCI device ID 判定。
 * QEMU 编号约定（virtio 1.0 spec §4.1.2.1 + QEMU hw/virtio/virtio-pci.c）:
 *   - transitional/legacy: 0x1000=net, 0x1001=blk, 0x1002=balloon, ...
 *     （历史 table，不是 0x1000+virtio_id；virtio device id 由 PCI Subsystem ID 携带）
 *   - modern-only: 0x1040+virtio_id → net = 0x1041
 * class code (0x02=Network) 作为第二判据，与 virtio-blk(0x1001/Storage) 区分。 */
static int virtio_is_net_device(u16 device_id) {
    return (device_id == 0x1000u || device_id == 0x1041u) ? 1 : 0;
}

static int virtio_net_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
    for (u8 bus = 0; bus < 16; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = pci_read(bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;

            u8 header = (u8)(pci_read(bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;
            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = pci_read(bus, dev, func, PCI_VENDOR_ID);
                u16 vendor = (u16)(vd2 & 0xffff);
                u16 device = (u16)(vd2 >> 16);
                if (vendor != PCI_VENDOR_VIRTIO) continue;

                u32 class_reg = pci_read(bus, dev, func, PCI_CLASS_REG);
                u8 class_code = (u8)(class_reg >> 24);
                if (virtio_is_net_device(device) && class_code == 0x02) {
                    *out_bus = bus;
                    *out_dev = dev;
                    *out_func = func;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static const char *virtio_cap_name(u8 cfg_type) {
    if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) return "common_cfg";
    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) return "notify_cfg";
    if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) return "isr_cfg";
    if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) return "device_cfg";
    if (cfg_type == VIRTIO_PCI_CAP_PCI_CFG) return "pci_cfg";
    return "unknown_cfg";
}

static u32 read_bar_raw(u8 bus, u8 dev, u8 func, u8 bar) {
    return pci_read(bus, dev, func, (u8)(PCI_BAR0 + bar * 4));
}

static void log_bars(u8 bus, u8 dev, u8 func) {
    for (u8 bar = 0; bar < 6; bar++) {
        u32 raw = read_bar_raw(bus, dev, func, bar);
        if (raw == 0 || raw == 0xffffffffu) continue;
        log_hex("[virtio_net] BAR index=", bar);
        log_hex("[virtio_net]   raw=", raw);
        if (raw & 1u) {
            log_hex("[virtio_net]   io base=", raw & 0xfffffffcU);
        } else {
            log_hex("[virtio_net]   mmio base=", raw & 0xfffffff0U);
        }
    }
}

static void log_virtio_caps(u8 bus, u8 dev, u8 func) {
    u8 cap = pci_read8(bus, dev, func, PCI_CAP_PTR) & 0xfc;
    u32 guard = 0;
    u32 found = 0;

    while (cap && guard++ < 32) {
        u8 cap_id = pci_read8(bus, dev, func, cap + 0);
        u8 next = pci_read8(bus, dev, func, cap + 1) & 0xfc;

        if (cap_id == 0x09) {
            u8 cfg_type = pci_read8(bus, dev, func, cap + 3);
            u8 bar = pci_read8(bus, dev, func, cap + 4);
            u32 offset = pci_read(bus, dev, func, cap + 8);
            u32 length = pci_read(bus, dev, func, cap + 12);
            found++;
            g_log->info("[virtio_net] virtio capability");
            g_log->info(virtio_cap_name(cfg_type));
            log_hex("[virtio_net]   cap_offset=", cap);
            log_hex("[virtio_net]   cfg_type=", cfg_type);
            log_hex("[virtio_net]   bar=", bar);
            log_hex("[virtio_net]   offset=", offset);
            log_hex("[virtio_net]   length=", length);
            if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                u32 mult = pci_read(bus, dev, func, cap + 16);
                log_hex("[virtio_net]   notify_off_multiplier=", mult);
            }
        }

        cap = next;
    }

    log_hex("[virtio_net] virtio cap count=", found);
    if (!found) {
        g_log->warn("[virtio_net] no modern virtio capabilities; transitional IO path expected");
    }
}

/* ================= legacy device 初始化序列 ================= */

/* device status 寄存器读写（8 位） */
static void vdev_set_status(u8 status) {
    dkm_outb((u16)(g_io_base + VP_STATUS), status);
}

static u8 vdev_get_status(void) {
    return dkm_inb((u16)(g_io_base + VP_STATUS));
}

/* reset → ACKNOWLEDGE → DRIVER → feature 协商 → FEATURES_OK。
 * 返回 0 成功；失败返回负数（调用方回退 stub 注册）。 */
static int vdev_negotiate(void) {
    /* 1. reset: 写 0 并等待设备读回 0（spec §4.1.4.3 要求轮询确认） */
    vdev_set_status(0);
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (vdev_get_status() != 0) {
        if (dkm_rdtsc() > deadline) {
            g_log->error("[virtio_net] device reset timeout");
            return -1;
        }
        __asm__ volatile("pause");
    }

    /* 2/3. ACKNOWLEDGE → DRIVER（status 位按序累积写入） */
    vdev_set_status(VS_ACKNOWLEDGE);
    vdev_set_status(VS_ACKNOWLEDGE | VS_DRIVER);

    /* 4. feature negotiation: 只取 MAC + STATUS 子集。
     * 不接受 MRG_RXBUF/CTRL_VQ/CSUM/GSO 等复杂特性 →
     * virtio_net_hdr 固定 10 字节、无控制队列、无卸载，数据路径最简。 */
    u32 dev_feat = dkm_inl((u16)(g_io_base + VP_HOST_FEATURES));
    log_hex("[virtio_net] device features=", dev_feat);
    u32 subset = dev_feat & (VNET_F_MAC | VNET_F_STATUS);
    dkm_outl((u16)(g_io_base + VP_GUEST_FEATURES), subset);
    log_hex("[virtio_net] guest features=", subset);

    /* 5. FEATURES_OK 并回读确认（spec §3.1.1）。
     * 纯 legacy（pre-1.0）设备没有该位概念；QEMU transitional 实现 1.0 语义会置位。
     * 未置位时按 legacy 兼容策略 warn 后继续（与 Linux legacy 驱动一致）。 */
    vdev_set_status(VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK);
    u8 st = vdev_get_status();
    log_hex("[virtio_net] status after FEATURES_OK=", st);
    if (!(st & VS_FEATURES_OK)) {
        g_log->warn("[virtio_net] FEATURES_OK not echoed; continuing as pre-1.0 legacy device");
    } else {
        g_log->info("[virtio_net] FEATURES_OK confirmed");
    }
    return 0;
}

/* QueuePFN = 物理地址 >> 12（legacy 队列地址寄存器语义） */
static void vn_outl_pfn(const struct virtq *q) {
    dkm_outl((u16)(g_io_base + VP_QUEUE_PFN), (u32)(q->phys >> 12));
}

/* 建立单个 virtqueue：读容量 → 计算 legacy 布局 → 分配连续页 → 写 QueuePFN。 */
static int vq_setup(const struct dkm_dma_api *dma, struct virtq *q, u16 qsel) {
    q->qsel = qsel;
    q->last_used_idx = 0;

    vn_outw((u16)(g_io_base + VP_QUEUE_SEL), qsel);
    q->size = vn_inw((u16)(g_io_base + VP_QUEUE_NUM));
    if (q->size == 0) {
        g_log->error("[virtio_net] queue size is 0");
        return -1;
    }

    /* legacy 布局（spec §2.4.2）:
     *   desc @ 0，avail @ 16*size，used @ 4KiB 对齐边界，整段物理连续。 */
    u64 avail_off = 16ULL * q->size;
    u64 used_off  = (avail_off + 6ULL + 2ULL * q->size + 4095ULL) & ~4095ULL;
    u64 total     = used_off + 6ULL + 8ULL * q->size;
    u64 pages     = (total + 4095ULL) / 4096ULL;

    struct dkm_dma_buffer buf;
    /* 低 4G + 页对齐：QueuePFN 只接受 phys>>12，alloc_pages 返回已清零页 */
    if (dma->alloc_pages(pages, 4096, 0x100000000ULL, &buf) != 0) {
        g_log->error("[virtio_net] queue memory alloc failed");
        return -2;
    }

    q->phys  = buf.phys;
    q->desc  = (struct vring_desc *)buf.virt;
    q->avail = (struct vring_avail *)((u8 *)buf.virt + avail_off);
    q->used  = (struct vring_used *)((u8 *)buf.virt + used_off);

    vn_outl_pfn(q);
    return 0;
}

/* kick 队列：写 queue index 到 QueueNotify */
static void vq_notify(const struct virtq *q) {
    vn_outw((u16)(g_io_base + VP_QUEUE_NOTIFY), q->qsel);
}

/* 把描述符挂到 avail ring 并推进 idx。
 * 顺序约束：desc 字段必须先写好（调用方责任），ring 写入 → fence → idx 推进。 */
static void vq_push_avail(struct virtq *q, u16 desc_idx) {
    u16 slot = (u16)(q->avail->idx % q->size);
    q->avail->ring[slot] = desc_idx;
    vn_fence();
    q->avail->idx = (u16)(q->avail->idx + 1u);
    vn_fence();
}

/* 从 used ring 取一个完成项。返回 0=取到，1=空。 */
static int vq_pop_used(struct virtq *q, u16 *out_id, u32 *out_len) {
    vn_fence();
    if (q->used->idx == q->last_used_idx) return 1;
    struct vring_used_elem *e = &q->used->ring[q->last_used_idx % q->size];
    *out_id  = (u16)e->id;
    *out_len = e->len;
    vn_fence();
    q->last_used_idx = (u16)(q->last_used_idx + 1u);
    return 0;
}

/* ================= RX 路径 ================= */

/* 回收 RX buffer：重置描述符并重新挂回 avail ring，保持 RX 水位。 */
static void rx_requeue(u16 id) {
    g_rxq.desc[id].addr  = g_rx_buf_phys[id];
    g_rxq.desc[id].len   = VNET_BUF_SIZE;
    g_rxq.desc[id].flags = VRING_DESC_F_WRITE;
    g_rxq.desc[id].next  = 0;
    vq_push_avail(&g_rxq, id);
    vq_notify(&g_rxq);
}

/* 预填全部 RX buffer（单描述符覆盖 hdr+帧，device 可写）。 */
static int rx_prefill(const struct dkm_dma_api *dma) {
    for (u32 i = 0; i < RX_BUF_COUNT; i++) {
        struct dkm_dma_buffer b;
        if (dma->alloc_pages(1, 4096, 0x100000000ULL, &b) != 0) {
            g_log->error("[virtio_net] rx buffer alloc failed");
            log_hex("[virtio_net]   filled=", i);
            return -1;
        }
        g_rx_buf[i]      = (u8 *)b.virt;
        g_rx_buf_phys[i] = b.phys;

        g_rxq.desc[i].addr  = b.phys;
        g_rxq.desc[i].len   = VNET_BUF_SIZE;
        g_rxq.desc[i].flags = VRING_DESC_F_WRITE;
        g_rxq.desc[i].next  = 0;
        g_rxq.avail->ring[i] = (u16)i;
    }
    vn_fence();
    g_rxq.avail->idx = (u16)RX_BUF_COUNT;
    vn_fence();
    log_hex("[virtio_net] rx buffers prefilled=", RX_BUF_COUNT);
    return 0;
}

/* netdev rx_poll 回调（契约与 e1000 一致）:
 * 返回 0 且 *out_length>0 = 收到一帧；1 = 无数据；负数 = 错误。 */
static int virtio_net_rx_poll(void *ctx, void *buffer, u32 capacity, u32 *out_length) {
    (void)ctx;
    if (out_length) *out_length = 0;
    if (!g_ready || !buffer || !out_length) return -1;

    u16 id;
    u32 total;
    if (vq_pop_used(&g_rxq, &id, &total) != 0) return 1;   /* 无完成项 */

    /* 防御：id 越界或总长小于 hdr → 丢帧（id 无效时无法回收，只计数） */
    if (id >= RX_BUF_COUNT) {
        g_stat_rx_err++;
        return -3;
    }
    if (total < VNET_HDR_SIZE) {
        g_stat_rx_err++;
        rx_requeue(id);
        return -3;
    }

    u32 frame_len = total - VNET_HDR_SIZE;   /* 剥掉 virtio_net_hdr */
    if (frame_len > capacity) {
        g_stat_rx_overflow++;
        rx_requeue(id);
        return -2;
    }

    copy_bytes(buffer, g_rx_buf[id] + VNET_HDR_SIZE, frame_len);
    *out_length = frame_len;
    rx_requeue(id);
    g_stat_rx_ok++;
    return 0;
}

/* ================= TX 路径 ================= */

/* 收割 TX used ring，把完成的 buffer 标回空闲。 */
static void tx_reclaim(void) {
    u16 id;
    u32 len;
    while (vq_pop_used(&g_txq, &id, &len) == 0) {
        (void)len;   /* TX 的 used.len 无语义，仅 id 用于回收 */
        if (id < TX_BUF_COUNT) g_tx_free[id] = 1;
    }
}

static int tx_buffers_alloc(const struct dkm_dma_api *dma) {
    for (u32 i = 0; i < TX_BUF_COUNT; i++) {
        struct dkm_dma_buffer b;
        if (dma->alloc_pages(1, 4096, 0x100000000ULL, &b) != 0) {
            g_log->error("[virtio_net] tx buffer alloc failed");
            return -1;
        }
        g_tx_buf[i]      = (u8 *)b.virt;
        g_tx_buf_phys[i] = b.phys;
        g_tx_free[i]     = 1;
    }
    return 0;
}

/* netdev tx 回调：hdr(10B 全零，无卸载) + 帧单描述符提交，kick 后同步等待完成。
 * 与 e1000 行为一致：TSC 1 秒超时兜底，避免上层协议栈无限阻塞。 */
static int virtio_net_tx(void *ctx, const void *packet, u32 length) {
    (void)ctx;
    if (!g_ready || !packet || length == 0 || length + VNET_HDR_SIZE > VNET_BUF_SIZE) {
        g_stat_tx_err++;
        return -1;
    }

    tx_reclaim();
    int slot = -1;
    for (u32 i = 0; i < TX_BUF_COUNT; i++) {
        if (g_tx_free[i]) { slot = (int)i; break; }
    }
    if (slot < 0) {
        g_stat_tx_err++;
        g_log->warn("[virtio_net] tx ring full");
        return -2;
    }

    u8 *buf = g_tx_buf[slot];
    zero_bytes(buf, VNET_HDR_SIZE);              /* virtio_net_hdr: flags/gso 全 0 */
    copy_bytes(buf + VNET_HDR_SIZE, packet, length);

    /* desc 索引与 buffer 1:1 映射，回收时 used.id 即 buffer 索引 */
    g_txq.desc[slot].addr  = g_tx_buf_phys[slot];
    g_txq.desc[slot].len   = length + VNET_HDR_SIZE;
    g_txq.desc[slot].flags = 0;                  /* device 只读 */
    g_txq.desc[slot].next  = 0;
    g_tx_free[slot] = 0;

    vq_push_avail(&g_txq, (u16)slot);
    vq_notify(&g_txq);

    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
    while (dkm_rdtsc() < deadline) {
        tx_reclaim();
        if (g_tx_free[slot]) {
            g_stat_tx_ok++;
            return 0;
        }
        __asm__ volatile("pause");
    }
    g_stat_tx_timeout++;
    g_log->warn("[virtio_net] tx timeout");
    log_hex("[virtio_net]   txq used idx=", g_txq.used->idx);
    log_hex("[virtio_net]   txq last_used=", g_txq.last_used_idx);
    return -3;
}

/* ================= IRQ（辅助统计，数据路径不依赖） ================= */

static int virtio_net_irq(u8 irq) {
    (void)irq;
    if (!g_io_base) return 0;
    /* ISR 读即清除；bit0=queue used buffer 通知，bit1=config change */
    u8 isr = dkm_inb((u16)(g_io_base + VP_ISR));
    if (isr & 0x1u) g_stat_irq_count++;
    return 0;   /* 不 suppress EOI，由 IDT 发 PIC EOI */
}

/* ================= boot 自测：DHCP DISCOVER/OFFER ================= */

#define ST_XID        0x4B1D3A5Fu
#define ST_FRAME_LEN  342u    /* 14 eth + 20 ip + 8 udp + 300 bootp */

/* 构造 DHCP DISCOVER（全手工大端字段，避免 packed struct 字节序歧义）。
 * BOOTP 载荷补齐到 300 字节（RFC 1542 最小值，slirp DHCP server 容忍度最高）。 */
static void st_build_discover(const u8 mac[6]) {
    u8 *f = g_st_frame;
    zero_bytes(f, ST_FRAME_LEN);

    /* Ethernet: 广播 DISCOVER */
    for (u32 i = 0; i < 6; i++) f[i] = 0xff;
    for (u32 i = 0; i < 6; i++) f[6 + i] = mac[i];
    f[12] = 0x08; f[13] = 0x00;                 /* IPv4 */

    /* IPv4 @14 */
    u8 *ip = f + 14;
    ip[0] = 0x45;                               /* v4, ihl=5 */
    ip[1] = 0x00;
    ip[2] = 0x01; ip[3] = 0x48;                 /* total len = 328 */
    ip[4] = 0x00; ip[5] = 0x00;                 /* id */
    ip[6] = 0x00; ip[7] = 0x00;                 /* flags/frag */
    ip[8] = 64;                                 /* ttl */
    ip[9] = 17;                                 /* UDP */
    ip[10] = 0x00; ip[11] = 0x00;               /* cksum 占位 */
    /* src 0.0.0.0 @12..15 保持 0 */
    ip[16] = 0xff; ip[17] = 0xff; ip[18] = 0xff; ip[19] = 0xff;

    /* IP header checksum（标准 1's complement，大端语义直接按字节对累加） */
    u32 sum = 0;
    for (u32 i = 0; i < 20; i += 2) sum += ((u32)ip[i] << 8) | ip[i + 1];
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    u16 ck = (u16)(~sum & 0xffffu);
    ip[10] = (u8)(ck >> 8); ip[11] = (u8)ck;

    /* UDP @34: 68 → 67，校验和置 0（IPv4 下合法） */
    u8 *udp = f + 34;
    udp[0] = 0x00; udp[1] = 68;
    udp[2] = 0x00; udp[3] = 67;
    udp[4] = 0x01; udp[5] = 0x34;               /* len = 308 */
    udp[6] = 0x00; udp[7] = 0x00;

    /* BOOTP @42（固定 236B + magic 4B + options，pad 到 300B） */
    u8 *bp = f + 42;
    bp[0] = 1;                                  /* op = BOOTREQUEST */
    bp[1] = 1;                                  /* htype = Ethernet */
    bp[2] = 6;                                  /* hlen */
    bp[3] = 0;                                  /* hops */
    bp[4] = (u8)(ST_XID >> 24);
    bp[5] = (u8)(ST_XID >> 16);
    bp[6] = (u8)(ST_XID >> 8);
    bp[7] = (u8)ST_XID;
    /* secs @8..9 = 0 */
    bp[10] = 0x80; bp[11] = 0x00;               /* flags = broadcast */
    /* ciaddr/yiaddr/siaddr/giaddr @12..27 = 0 */
    for (u32 i = 0; i < 6; i++) bp[28 + i] = mac[i];   /* chaddr（16B，后 10B 为 0） */
    /* sname @44..107, file @108..235 = 0 */
    bp[236] = 0x63; bp[237] = 0x82; bp[238] = 0x53; bp[239] = 0x63;  /* magic cookie */
    u8 *opt = bp + 240;
    opt[0] = 53; opt[1] = 1; opt[2] = 1;        /* DHCP msg type = DISCOVER */
    opt[3] = 55; opt[4] = 4;                    /* param request list */
    opt[5] = 1;  opt[6] = 3;  opt[7] = 6;  opt[8] = 15;   /* subnet/router/dns/domain */
    opt[9] = 255;                               /* end */
}

/* 解析帧是否为匹配 XID 的 DHCP OFFER。
 * 返回 2=OFFER（*out_yiaddr 有效，主机序），5=ACK，0=无关帧。 */
static int st_parse_reply(const u8 *f, u32 len, u32 *out_yiaddr) {
    if (len < 14 + 20 + 8 + 240) return 0;
    if (f[12] != 0x08 || f[13] != 0x00) return 0;         /* 非 IPv4 */
    u32 ihl = (u32)(f[14] & 0x0f) * 4u;
    if (ihl < 20) return 0;
    if (f[14 + 9] != 17) return 0;                        /* 非 UDP */
    u32 udp = 14 + ihl;
    if (len < udp + 8 + 240) return 0;
    u16 dport = (u16)(((u16)f[udp + 2] << 8) | f[udp + 3]);
    if (dport != 68) return 0;                            /* 非 DHCP 回包 */

    u32 bp = udp + 8;
    if (f[bp] != 2) return 0;                             /* 非 BOOTREPLY */
    u32 xid = ((u32)f[bp + 4] << 24) | ((u32)f[bp + 5] << 16) |
              ((u32)f[bp + 6] << 8) | f[bp + 7];
    if (xid != ST_XID) return 0;                          /* XID 不匹配 */
    if (f[bp + 236] != 0x63 || f[bp + 237] != 0x82 ||
        f[bp + 238] != 0x53 || f[bp + 239] != 0x63) return 0;

    u32 yiaddr = ((u32)f[bp + 16] << 24) | ((u32)f[bp + 17] << 16) |
                 ((u32)f[bp + 18] << 8) | f[bp + 19];

    /* options 区扫描 msg type（53） */
    u32 i = bp + 240;
    while (i < len) {
        u8 o = f[i];
        if (o == 255) break;
        if (o == 0) { i++; continue; }
        if (i + 1 >= len) break;
        u8 olen = f[i + 1];
        if (o == 53 && olen == 1 && i + 2 < len) {
            u8 mtype = f[i + 2];
            if (mtype == 2 || mtype == 5) {
                *out_yiaddr = yiaddr;
                return mtype;
            }
            return 0;
        }
        i += 2u + olen;
    }
    return 0;
}

/* boot 自测：经真实网络后端验证 TX（desc+notify+used 回收）与
 * RX（avail 预填 + used 收割 + hdr 剥离）全链路。
 * 任何失败只记日志，绝不阻塞/崩溃启动流程。 */
static void virtio_net_selftest(void) {
    if (!g_ready) return;
    g_log->info("[virtio_net] selftest begin: DHCP DISCOVER -> expect OFFER from backend");

    st_build_discover(g_mac);
    int txrc = virtio_net_tx(0, g_st_frame, ST_FRAME_LEN);
    log_hex("[virtio_net] selftest tx rc=", (u64)(i64)txrc);
    if (txrc != 0) {
        g_log->warn("[virtio_net] selftest: TX path FAILED");
        return;
    }
    g_log->info("[virtio_net] selftest: TX path ok (descriptor consumed by device)");

    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 2000;
    u32 frames = 0;
    while (dkm_rdtsc() < deadline) {
        u32 flen = 0;
        int rc = virtio_net_rx_poll(0, g_st_buf, sizeof(g_st_buf), &flen);
        if (rc == 0 && flen > 0) {
            frames++;
            u32 ethertype = ((u32)g_st_buf[12] << 8) | g_st_buf[13];
            log_hex("[virtio_net] selftest rx frame len=", flen);
            log_hex("[virtio_net]   ethertype=", ethertype);
            u32 yiaddr = 0;
            int t = st_parse_reply(g_st_buf, flen, &yiaddr);
            if (t == 2) {
                log_ipv4("[virtio_net] selftest: DHCP OFFER yiaddr=", yiaddr);
                g_log->info("[virtio_net] selftest PASS: RX path ok (frame from real backend)");
                return;
            }
        }
        __asm__ volatile("pause");
    }
    g_log->warn("[virtio_net] selftest: no DHCP OFFER within 2000ms");
    log_hex("[virtio_net] selftest rx frames seen=", frames);
}

/* ================= stub（初始化失败时的安全占位，与历史行为一致） ================= */

static int virtio_net_tx_stub(void *ctx, const void *packet, u32 length) {
    (void)ctx;
    (void)packet;
    log_hex("[virtio_net] tx stub: packet dropped, len=", length);
    return 0;
}

static int virtio_net_rx_poll_stub(void *ctx, void *buffer, u32 capacity, u32 *out_length) {
    (void)ctx;
    (void)buffer;
    (void)capacity;
    if (out_length) *out_length = 0;
    return 0;
}

/* ================= driver 入口 ================= */

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    /* 实机要求: 在驱动初始化开头校准 TSC, 为后续时序提供准确计时 */
    dkm_tsc_calibrate();

    g_log->info("[virtio_net] init begin");

    u8 bus = 0;
    u8 dev = 0;
    u8 func = 0;
    if (virtio_net_find(&bus, &dev, &func) != 0) {
        g_log->warn("[virtio_net] virtio-net device not found");
        g_log->info("[virtio_net] driver ready");
        return 0;
    }

    g_log->info("[virtio_net] virtio-net device found");
    log_hex("[virtio_net] bus=", bus);
    log_hex("[virtio_net] dev=", dev);
    log_hex("[virtio_net] func=", func);

    u32 vd = pci_read(bus, dev, func, PCI_VENDOR_ID);
    u16 vendor = (u16)(vd & 0xffff);
    u16 device = (u16)(vd >> 16);
    u32 class_reg = pci_read(bus, dev, func, PCI_CLASS_REG);
    u8 revision = (u8)(class_reg & 0xff);

    log_hex("[virtio_net] vendor=", vendor);
    log_hex("[virtio_net] device=", device);
    log_hex("[virtio_net] revision=", revision);

    u32 command = pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUSM;
    pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[virtio_net] PCI command=", command);

    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[virtio_net] PCI IRQ line=", irq_line);

    log_bars(bus, dev, func);
    log_virtio_caps(bus, dev, func);

    /* ---- legacy I/O BAR 检查：transitional 设备 BAR0 必须是 I/O space ---- */
    u32 bar0 = pci_read(bus, dev, func, PCI_BAR0);
    int datapath_ok = 0;

    if (!(bar0 & 1u)) {
        g_log->warn("[virtio_net] BAR0 is not I/O space (modern-only device?); legacy path unavailable");
    } else if (!api->dma || !api->dma->alloc_pages) {
        g_log->warn("[virtio_net] dma API unavailable");
    } else {
        g_io_base = (u16)(bar0 & 0xfffcU);
        log_hex("[virtio_net] legacy io_base=", g_io_base);

        if (vdev_negotiate() != 0) {
            g_log->warn("[virtio_net] feature negotiation failed");
        } else if (vq_setup(api->dma, &g_rxq, 0) != 0) {
            g_log->warn("[virtio_net] RXQ setup failed");
        } else if (vq_setup(api->dma, &g_txq, 1) != 0) {
            g_log->warn("[virtio_net] TXQ setup failed");
        } else if (rx_prefill(api->dma) != 0) {
            g_log->warn("[virtio_net] RX prefill failed");
        } else if (tx_buffers_alloc(api->dma) != 0) {
            g_log->warn("[virtio_net] TX buffer alloc failed");
        } else {
            log_hex("[virtio_net] rxq size=", g_rxq.size);
            log_hex("[virtio_net] rxq phys=", g_rxq.phys);
            log_hex("[virtio_net] txq size=", g_txq.size);
            log_hex("[virtio_net] txq phys=", g_txq.phys);

            /* MAC: device config @ io_base+0x14（协商 MAC feature 后有效） */
            for (u32 i = 0; i < 6; i++) {
                g_mac[i] = dkm_inb((u16)(g_io_base + VP_NET_CFG_MAC + i));
            }
            u8 mac_sum = 0;
            for (u32 i = 0; i < 6; i++) mac_sum |= g_mac[i];
            if (mac_sum == 0 || (g_mac[0] & 1u)) {
                /* 全 0 或组播位异常 → fallback */
                g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
                g_mac[3] = 0x12; g_mac[4] = 0x34; g_mac[5] = 0x57;
                g_log->warn("[virtio_net] invalid device MAC, using fallback");
            }
            log_mac("[virtio_net] MAC=", g_mac);

            /* DRIVER_OK：设备开始处理队列；随后 kick RXQ 让设备吃掉预填 buffer */
            vdev_set_status(VS_ACKNOWLEDGE | VS_DRIVER | VS_FEATURES_OK | VS_DRIVER_OK);
            vq_notify(&g_rxq);
            g_ready = 1;
            datapath_ok = 1;
            g_log->info("[virtio_net] DRIVER_OK set; virtqueues live");
        }
    }

    /* ---- IRQ 注册（辅助统计；数据路径为纯轮询，不依赖中断） ----
     * 注意: kernel irq_register 按 IDT vector 索引（PIC IRQn → vector 0x20+n），
     * 传裸 IRQ 号会落到异常向量槽位永不触发（e1000 当前如此）。
     * PIC 未解屏蔽该线时自动退化为纯轮询。 */
    if (datapath_ok && api->irq_register && irq_line > 0 && irq_line < 16) {
        api->irq_register((u8)(irq_line + 0x20u), (void *)(uintptr_t)virtio_net_irq);
        g_log->info("[virtio_net] IRQ handler registered (vector = irq + 0x20)");
    } else {
        g_log->info("[virtio_net] IRQ not registered (polling mode)");
    }

    /* ---- 注册 netdev ---- */
    if (api->net && api->net->register_device) {
        struct dkm_net_device_desc netdev;
        zero_bytes(&netdev, sizeof(netdev));
        netdev.name = "virtio-net";
        for (u32 i = 0; i < 6; i++) netdev.mac[i] = g_mac[i];
        netdev.ctx = 0;
        if (datapath_ok) {
            netdev.flags   = DKM_NET_F_LINK_UP | DKM_NET_F_TX_READY | DKM_NET_F_RX_READY;
            netdev.tx      = virtio_net_tx;
            netdev.rx_poll = virtio_net_rx_poll;
        } else {
            /* 安全占位：flags 仅 LINK_UP，netman 不会选它跑 DHCP（与历史行为一致） */
            netdev.flags   = DKM_NET_F_LINK_UP;
            netdev.tx      = virtio_net_tx_stub;
            netdev.rx_poll = virtio_net_rx_poll_stub;
        }
        int net_index = api->net->register_device(&netdev);
        log_hex("[virtio_net] netdev registered, index=", (u64)(i64)net_index);
    } else {
        g_log->warn("[virtio_net] net API unavailable; netdev not registered");
    }

    /* ---- boot 自测（证明 TX/RX 全链路，失败仅记日志） ---- */
    virtio_net_selftest();

    log_hex("[virtio_net] stat tx_ok=", g_stat_tx_ok);
    log_hex("[virtio_net] stat rx_ok=", g_stat_rx_ok);
    g_log->info("[virtio_net] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    /* reset 设备：停止一切 DMA 与队列活动 */
    if (g_io_base) {
        vdev_set_status(0);
    }
    g_ready = 0;
    g_io_base = 0;
    return 0;
}
