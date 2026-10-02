/* DKM e1000 Driver — Intel 8254x/8257x Gigabit Ethernet
 * Stage 3, optional, depends on "pci" and "irq", provides "netdev".
 * Supports classic e1000 (82540EM/82545/82546) and e1000e (82574L/I217/I218/I219).
 * Finds device via PCI, maps MMIO, reads MAC (RAL/RAH or EEPROM),
 * initializes RX/TX rings, provides link/speed/duplex diagnostics.
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(e1k_irq_count);
DKM_STAT_DECL(e1k_rx_packets);
DKM_STAT_DECL(e1k_tx_packets);

static const char *const g_depends[] = {"pci","irq"};
static const char *const g_provides[] = {"netdev"};
__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC,.abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="e1000",.version="0.2.0",.vendor="Deshab",
    .driver_class=9,.stage=3,.flags=0,.priority=0,
    .depends=g_depends,.depends_count=2,
    .provides=g_provides,.provides_count=1,.min_kernel_abi=1,
};

/* e1000 MMIO registers */
#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8
#define E1000_RAL     0x5400
#define E1000_RAH     0x5404
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_RXDCTL  0x2828
#define E1000_RCTL    0x0100
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_TXDCTL  0x3828
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_MANC    0x0580

/* STATUS register bits (link/speed/duplex diagnostic) */
#define E1000_STATUS_FD         (1u << 0)   /* full duplex */
#define E1000_STATUS_LU         (1u << 1)   /* link up */
#define E1000_STATUS_FUNC_SHIFT 2           /* PCI function number (ro) */
#define E1000_STATUS_TXOFF      (1u << 4)   /* transmitter paused */
#define E1000_STATUS_SPEEDMASK  (3u << 6)
#define E1000_STATUS_SPEED_10   (0u << 6)
#define E1000_STATUS_SPEED_100  (1u << 6)
#define E1000_STATUS_SPEED_1000 (2u << 6)   /* classic e1000 only */

/* CTRL register bits */
#define E1000_CTRL_FD       (1u << 0)
#define E1000_CTRL_SLU      (1u << 6)
#define E1000_CTRL_RST      (1u << 26)      /* software reset */
#define E1000_CTRL_ASDE     (1u << 11)      /* auto-speed detect enable */
#define E1000_CTRL_FRCSPD   (1u << 12)      /* force speed */
#define E1000_CTRL_FRCDPLX  (1u << 13)      /* force duplex */

/* e1000e (82574L/I217/I219) speed encoding in STATUS differs */
#define E1000_STATUS_SPEED_1000_E1000E (3u << 6)

#define E1000_RX_DESC_COUNT 8u
#define E1000_TX_DESC_COUNT 8u
#define E1000_RX_BUF_SIZE   2048u
#define E1000_TX_BUF_SIZE   2048u

#define E1000_RCTL_EN       (1u << 1)
#define E1000_RCTL_SBP      (1u << 2)
#define E1000_RCTL_UPE      (1u << 3)
#define E1000_RCTL_MPE      (1u << 4)
#define E1000_RCTL_BAM      (1u << 15)
#define E1000_RCTL_SECRC    (1u << 26)
#define E1000_RCTL_BSIZE_2048 0u

#define E1000_TCTL_EN       (1u << 1)
#define E1000_TCTL_PSP      (1u << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_TX_CMD_EOP    (1u << 0)
#define E1000_TX_CMD_IFCS   (1u << 1)
#define E1000_TX_CMD_RS     (1u << 3)
#define E1000_TX_STA_DD     (1u << 0)
#define E1000_RX_STA_DD     (1u << 0)

/* ---- e1000 device ID table ---- */
/* 分类: 0=经典 e1000 (8254x), 1=e1000e (8257x/I217/I218/I219) */
struct e1000_device_id {
    u16 did;
    u8  family;     /* 0=classic, 1=e1000e */
    const char *name;
};

static const struct e1000_device_id g_e1000_ids[] = {
    /* 经典 e1000 (8254x 系列) — EERD 接口兼容 */
    {0x100E, 0, "82540EM"},       /* QEMU e1000 default */
    {0x1004, 0, "82543GC"},       /* Copper */
    {0x100F, 0, "82545EM"},       /* Copper */
    {0x1010, 0, "82546EB"},       /* Copper dual-port */
    {0x1011, 0, "82545EM-Fiber"},
    {0x1012, 0, "82546EB-Fiber"},
    {0x1013, 0, "82541EI"},
    {0x1015, 0, "82540EM-LOM"},
    {0x1016, 0, "82540EP"},
    {0x1017, 0, "82540EP-LOM"},
    {0x1018, 0, "82541EI-LOM"},
    {0x1019, 0, "82547EI"},
    {0x101A, 0, "82547EI-LOM"},
    {0x101D, 0, "82546EB-Quad"},
    {0x1026, 0, "82545GM"},
    {0x1027, 0, "82545GM-Fiber"},
    {0x1028, 0, "82545GM-SerDes"},
    {0x1075, 0, "82547GI"},
    {0x1076, 0, "82541GI"},
    {0x1077, 0, "82541GI-LOM"},
    {0x1078, 0, "82541ER"},
    {0x1079, 0, "82546GB"},
    {0x107A, 0, "82546GB-Fiber"},
    {0x107B, 0, "82546GB-SerDes"},
    {0x107C, 0, "82541GI-Mobile"},
    {0x108B, 0, "82573E"},
    {0x108C, 0, "82573E-IAMT"},
    {0x109A, 0, "82573L"},

    /* e1000e (8257x/I217/I218/I219) 系列 — 寄存器兼容但 EERD/STATUS 编码有差异 */
    {0x10D3, 1, "82574L"},        /* QEMU e1000e default */
    {0x10F5, 1, "82578DM"},
    {0x10F6, 1, "82578DC"},
    {0x1502, 1, "82579V"},
    {0x1503, 1, "82579V-2"},
    {0x1533, 1, "I217-V"},
    {0x1534, 1, "I217-LM"},
    {0x153A, 1, "I218-V"},
    {0x153B, 1, "I218-LM"},
    {0x156F, 1, "I219-V"},
    {0x1570, 1, "I219-LM"},
    {0x157B, 1, "I219-V-2"},
    {0x157A, 1, "I219-LM-2"},
    {0x15D3, 1, "I219-V-3"},
    {0x15D8, 1, "I219-LM-3"},
    {0x15E3, 1, "I219-V-4"},
    {0x15D6, 1, "I219-V-5"},
    {0x15BD, 1, "I219-V-6"},
    {0x15B8, 1, "I219-LM-4"},
    {0x15BB, 1, "I219-LM-5"},
    {0x15DF, 1, "I219-LM-6"},
    {0x15E0, 1, "I219-V-7"},
    {0x15E1, 1, "I219-LM-7"},
    {0x15E2, 1, "I219-LM-8"},
    {0x15E4, 1, "I219-V-8"},
    {0x15E5, 1, "I219-V-9"},
    {0x15F9, 1, "I219-LM-9"},
    {0x15FA, 1, "I219-V-10"},
    {0x15FB, 1, "I219-LM-10"},
    {0x15FC, 1, "I219-LM-11"},
    {0x15FD, 1, "I219-V-11"},
    {0x15FE, 1, "I219-LM-12"},
    {0x15FF, 1, "I219-V-12"},

    /* I210/I211 (igb 系列) — 与 e1000e MMIO 兼容 */
    {0x1537, 1, "I210"},
    {0x1536, 1, "I211-AT"},

    /* 82575/82576/82580 (igb 系列, 多队列, 基本兼容) */
    {0x10A7, 1, "82575EB"},
    {0x10A9, 1, "82575EB-SerDes"},
    {0x10C9, 1, "82576"},
    {0x10E6, 1, "82576-Fiber"},
    {0x10E7, 1, "82576-Quad"},
    {0x150D, 1, "82580"},
    {0x1516, 1, "82580-Fiber"},
    {0x1526, 1, "82576-2"},

    {0, 0, 0}  /* 终止符 */
};

struct e1000_rx_desc {
    u64 addr;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
    u64 addr;
    u16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    u16 special;
} __attribute__((packed));

/* ---- 统计计数器（用于真机诊断） ---- */
static u64 g_stat_tx_ok;       /* TX 成功帧数 */
static u64 g_stat_tx_err;      /* TX 失败帧数 */
static u64 g_stat_tx_timeout;  /* TX 超时次数 */
static u64 g_stat_rx_ok;       /* RX 成功帧数 */
static u64 g_stat_rx_err;      /* RX 错误帧数 */
static u64 g_stat_rx_overflow; /* RX buffer 太小丢包 */
static u64 g_stat_irq_count;   /* IRQ 中断次数 */

/* 前向声明 */
static void raw_log(const char *s);
static void raw_hex(const char *prefix, u64 v);

/* 每 256 次数据路径事件打印一行摘要（避免刷串口）。 */
static void net_stat_tick(void) {
    u64 sum = g_stat_tx_ok + g_stat_tx_err + g_stat_tx_timeout +
              g_stat_rx_ok + g_stat_rx_err + g_stat_rx_overflow;
    if ((sum & 0xFF) != 0) return;
    raw_log("[e1000.raw] stat tx_ok=");
    raw_hex("", g_stat_tx_ok);
    raw_log("[e1000.raw] stat tx_err=");
    raw_hex("", g_stat_tx_err);
    raw_log("[e1000.raw] stat rx_ok=");
    raw_hex("", g_stat_rx_ok);
    raw_log("[e1000.raw] stat rx_err=");
    raw_hex("", g_stat_rx_err);
    raw_log("[e1000.raw] stat rx_overflow=");
    raw_hex("", g_stat_rx_overflow);
}

static const struct dkm_log_api *g_log;
static volatile u32 *g_mmio;
static u64 g_bar0_phys;
static struct e1000_rx_desc *g_rx_desc;
static struct e1000_tx_desc *g_tx_desc;
static u64 g_rx_desc_phys;
static u64 g_tx_desc_phys;
static u8 *g_rx_buf[E1000_RX_DESC_COUNT];
static u64 g_rx_buf_phys[E1000_RX_DESC_COUNT];
static u8 *g_tx_buf[E1000_TX_DESC_COUNT];
static u64 g_tx_buf_phys[E1000_TX_DESC_COUNT];
static u32 g_rx_tail;
static u32 g_tx_tail;
static int g_rings_ready;
static u8 g_device_family;   /* 0=classic e1000, 1=e1000e */
static u16 g_device_id;      /* PCI device ID */

#define COM1 0x3F8

/* 串口发送等待 thin wrapper — 调用 dkm 共享实现 */
static void serial_wait_tx(void) { dkm_serial_wait_tx(COM1); }

static void raw_log(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            serial_wait_tx();
            dkm_outb(COM1, '\r');
        }
        serial_wait_tx();
        dkm_outb(COM1, (u8)c);
    }
}

static void raw_hex(const char *prefix, u64 v) {
    static const char h[] = "0123456789abcdef";
    raw_log(prefix);
    raw_log("0x");
    for (int i=15; i>=0; i--) {
        serial_wait_tx();
        dkm_outb(COM1, (u8)h[(v >> (i * 4)) & 0xf]);
    }
    raw_log("\n");
}

/* 打印 MAC 地址 */
static void raw_mac(const char *prefix, const u8 mac[6]) {
    static const char h[] = "0123456789abcdef";
    raw_log(prefix);
    for (int i = 0; i < 6; i++) {
        serial_wait_tx();
        dkm_outb(COM1, (u8)h[(mac[i] >> 4) & 0xf]);
        serial_wait_tx();
        dkm_outb(COM1, (u8)h[mac[i] & 0xf]);
        if (i < 5) {
            serial_wait_tx();
            dkm_outb(COM1, ':');
        }
    }
    raw_log("\n");
}

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    return dkm_pci_read(bus, dev, func, reg);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 v) {
    dkm_pci_write(bus, dev, func, reg, v);
}

/* 根据 PCI Device ID 查找设备信息 */
static const struct e1000_device_id *e1000_lookup_id(u16 did) {
    for (int i = 0; g_e1000_ids[i].did != 0; i++) {
        if (g_e1000_ids[i].did == did) return &g_e1000_ids[i];
    }
    return 0;
}

/* 多总线扫描 e1000 设备。真机网卡可能在 bus>0 上。 */
static int e1000_find(u8 *out_bus, u8 *out_dev, u8 *out_func,
                      const struct e1000_device_id **out_id) {
    for (u8 bus = 0; bus < 16; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = pci_read(bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;
            if ((vd & 0xffff) != 0x8086) continue;

            /* 检查是否为多功能设备 */
            u8 header = (u8)(pci_read(bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;

            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = pci_read(bus, dev, func, PCI_VENDOR_ID);
                if ((vd2 & 0xffff) == 0xffff) continue;
                if ((vd2 & 0xffff) != 0x8086) continue;
                u16 did = (u16)(vd2 >> 16);

                const struct e1000_device_id *id = e1000_lookup_id(did);
                if (id) {
                    *out_bus = bus;
                    *out_dev = dev;
                    *out_func = func;
                    if (out_id) *out_id = id;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* 读取 EEPROM 一个 16-bit 字（经典 e1000 EERD 接口）。
 * 经典 e1000: EERD bit0=START, addr<<8, 完成后 bit4=DONE, data 在高 16 位。
 * e1000e: EERD 接口类似但地址字段位置不同 (addr<<2, bit1=START, bit2=DONE)。
 * 失败返回 0xffff。 */
static u16 e1000_eeprom_read(u16 addr) {
    if (g_device_family == 1) {
        /* e1000e: EERD addr<<2 | START(bit1), DONE=bit2, data=bits[31:16] */
        g_mmio[E1000_EERD/4] = ((u32)addr << 2) | 0x2u;
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
        while (dkm_rdtsc() < deadline) {
            u32 v = g_mmio[E1000_EERD/4];
            if (v & (1u << 2)) return (u16)(v >> 16);
            __asm__ volatile("pause");
        }
    } else {
        /* 经典 e1000: EERD addr<<8 | START(bit0), DONE=bit4 */
        g_mmio[E1000_EERD/4] = ((u32)addr << 8) | 1u;
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
        while (dkm_rdtsc() < deadline) {
            u32 v = g_mmio[E1000_EERD/4];
            if (v & (1u << 4)) return (u16)(v >> 16);
            __asm__ volatile("pause");
        }
    }
    return 0xffff;
}

/* 检测链路状态，返回 0=down, 1=up */
static int e1000_link_up(void) {
    if (!g_mmio) return 0;
    return (g_mmio[E1000_STATUS/4] & E1000_STATUS_LU) ? 1 : 0;
}

/* 读取当前速度 (Mbps)，返回 10/100/1000/0(未知) */
static u32 e1000_link_speed(void) {
    if (!g_mmio) return 0;
    u32 status = g_mmio[E1000_STATUS/4];
    u32 speed_bits = status & E1000_STATUS_SPEEDMASK;
    if (g_device_family == 1) {
        /* e1000e speed encoding: 00=reserved, 01=100, 10=1000, 11=1000 */
        if (speed_bits == E1000_STATUS_SPEED_1000_E1000E ||
            speed_bits == (2u << 6))
            return 1000;
        if (speed_bits == E1000_STATUS_SPEED_100) return 100;
        if (speed_bits == E1000_STATUS_SPEED_10) return 10;
        return 0;
    } else {
        /* 经典 e1000 speed encoding: 00=10, 01=100, 10=1000 */
        if (speed_bits == E1000_STATUS_SPEED_1000) return 1000;
        if (speed_bits == E1000_STATUS_SPEED_100) return 100;
        if (speed_bits == E1000_STATUS_SPEED_10) return 10;
        return 0;
    }
}

/* 读取当前双工模式，返回 1=全双工, 0=半双工 */
static int e1000_link_full_duplex(void) {
    if (!g_mmio) return 0;
    return (g_mmio[E1000_STATUS/4] & E1000_STATUS_FD) ? 1 : 0;
}

/* 打印链路诊断信息 */
static void e1000_log_link_status(void) {
    int up = e1000_link_up();
    if (up) {
        u32 speed = e1000_link_speed();
        int fd = e1000_link_full_duplex();
        const char *speed_str = "unknown";
        char speed_buf[8];
        if (speed == 1000) speed_str = "1000";
        else if (speed == 100) speed_str = "100";
        else if (speed == 10) speed_str = "10";
        else {
            /* 手动转数字 */
            speed_buf[0] = '0' + (char)(speed / 1000 % 10);
            speed_buf[1] = '0' + (char)(speed / 100 % 10);
            speed_buf[2] = '0' + (char)(speed / 10 % 10);
            speed_buf[3] = '0' + (char)(speed % 10);
            speed_buf[4] = 0;
            speed_str = speed_buf;
        }
        raw_log("[e1000] link=up speed=");
        raw_log(speed_str);
        raw_log("Mbps duplex=");
        raw_log(fd ? "full" : "half");
        raw_log("\n");
    } else {
        raw_log("[e1000] link=down\n");
    }
    raw_hex("[e1000] STATUS=", g_mmio ? g_mmio[E1000_STATUS/4] : 0);
}

/* 等待链路就绪（最长 2 秒），实机 PHY 自动协商可能需要时间 */
static int e1000_wait_for_link(void) {
    if (!g_mmio) return -1;
    /* 设置 ASDE (Auto-Speed Detect Enable) 让 PHY 自动协商 */
    if (g_device_family == 1) {
        /* e1000e: 建议先清除 FRCSPD/FRCDPLX，让自动协商生效 */
        u32 ctrl = g_mmio[E1000_CTRL/4];
        ctrl &= ~(E1000_CTRL_FRCSPD | E1000_CTRL_FRCDPLX);
        ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
        g_mmio[E1000_CTRL/4] = ctrl;
    }
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 2000;
    while (dkm_rdtsc() < deadline) {
        if (e1000_link_up()) return 0;
        __asm__ volatile("pause");
    }
    return -1;  /* 链路未就绪但驱动继续初始化（后续可能 link up） */
}

static int irq_handler(u8 irq) {
    (void)irq;
    if (!g_mmio) return 0;
    u32 icr = g_mmio[E1000_ICR/4];
    if (icr) {
        g_stat_irq_count++;
        DKM_STAT_INC(e1k_irq_count);
        g_mmio[E1000_ICR/4] = icr; /* clear */
    }
    return 0;
}

static void copy_bytes(void *dst, const void *src, u32 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i=0;i<len;i++) d[i]=s[i];
}

static void zero_bytes(void *dst, u32 len) {
    u8 *d = (u8 *)dst;
    for (u32 i=0;i<len;i++) d[i]=0;
}

static void mmio_flush(void) {
    if (g_mmio) (void)g_mmio[E1000_STATUS/4];
}

static void dma_fence(void) {
    __asm__ volatile("" ::: "memory");
}

static int e1000_init_rings(const struct dkm_dma_api *dma) {
    if (!dma || !dma->alloc_pages || !g_mmio) return -1;

    struct dkm_dma_buffer rx_ring, tx_ring;
    if (dma->alloc_pages(1, 16, 0x100000000ULL, &rx_ring) != 0) return -2;
    if (dma->alloc_pages(1, 16, 0x100000000ULL, &tx_ring) != 0) return -3;
    g_rx_desc = (struct e1000_rx_desc *)rx_ring.virt;
    g_tx_desc = (struct e1000_tx_desc *)tx_ring.virt;
    g_rx_desc_phys = rx_ring.phys;
    g_tx_desc_phys = tx_ring.phys;

    for (u32 i=0;i<E1000_RX_DESC_COUNT;i++) {
        struct dkm_dma_buffer b;
        if (dma->alloc_pages(1, 16, 0x100000000ULL, &b) != 0) return -4;
        g_rx_buf[i] = (u8 *)b.virt;
        g_rx_buf_phys[i] = b.phys;
        zero_bytes(&g_rx_desc[i], sizeof(g_rx_desc[i]));
        g_rx_desc[i].addr = b.phys;
    }

    for (u32 i=0;i<E1000_TX_DESC_COUNT;i++) {
        struct dkm_dma_buffer b;
        if (dma->alloc_pages(1, 16, 0x100000000ULL, &b) != 0) return -5;
        g_tx_buf[i] = (u8 *)b.virt;
        g_tx_buf_phys[i] = b.phys;
        zero_bytes(&g_tx_desc[i], sizeof(g_tx_desc[i]));
        g_tx_desc[i].addr = b.phys;
        g_tx_desc[i].status = E1000_TX_STA_DD;
    }
    dma_fence();
    raw_log("[e1000] ring/buffers allocated\n");

    g_mmio[E1000_RCTL/4] = 0;
    g_mmio[E1000_TCTL/4] = 0;
    mmio_flush();

    g_mmio[E1000_RDBAL/4] = (u32)(g_rx_desc_phys & 0xffffffffu);
    g_mmio[E1000_RDBAH/4] = (u32)(g_rx_desc_phys >> 32);
    g_mmio[E1000_RDLEN/4] = E1000_RX_DESC_COUNT * sizeof(struct e1000_rx_desc);
    g_mmio[E1000_RDH/4] = 0;
    g_rx_tail = E1000_RX_DESC_COUNT - 1;
    g_mmio[E1000_RDT/4] = g_rx_tail;

    g_mmio[E1000_TDBAL/4] = (u32)(g_tx_desc_phys & 0xffffffffu);
    g_mmio[E1000_TDBAH/4] = (u32)(g_tx_desc_phys >> 32);
    g_mmio[E1000_TDLEN/4] = E1000_TX_DESC_COUNT * sizeof(struct e1000_tx_desc);
    g_mmio[E1000_TDH/4] = 0;
    g_tx_tail = 0;
    g_mmio[E1000_TDT/4] = 0;

    /* e1000e/部分实现需要显式打开队列描述符控制；经典 e1000 会忽略/兼容。 */
    g_mmio[E1000_RXDCTL/4] = (1u << 25);
    g_mmio[E1000_TXDCTL/4] = (1u << 25);

    g_mmio[E1000_TIPG/4] = 0x0060200Au;
    dma_fence();
    g_mmio[E1000_RCTL/4] = E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048;
    g_mmio[E1000_TCTL/4] = E1000_TCTL_EN | E1000_TCTL_PSP | (0x0Fu << E1000_TCTL_CT_SHIFT) | (0x40u << E1000_TCTL_COLD_SHIFT);
    mmio_flush();

    g_rings_ready = 1;
    raw_log("[e1000] RX/TX rings initialized\n");
    return 0;
}

static int e1000_tx(void *ctx, const void *packet, u32 length) {
    (void)ctx;
    if (!g_rings_ready || !packet || length == 0 || length > E1000_TX_BUF_SIZE) {
        g_stat_tx_err++;
        return -1;
    }
    u32 index = g_tx_tail;
    if (!(g_tx_desc[index].status & E1000_TX_STA_DD)) {
        g_stat_tx_err++;
        return -2;  /* TX ring full */
    }
    copy_bytes(g_tx_buf[index], packet, length);
    dma_fence();
    g_tx_desc[index].length = (u16)length;
    g_tx_desc[index].cso = 0;
    g_tx_desc[index].cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    g_tx_desc[index].css = 0;
    g_tx_desc[index].special = 0;
    g_tx_desc[index].status = 0;
    dma_fence();
    g_tx_tail = (index + 1) % E1000_TX_DESC_COUNT;
    g_mmio[E1000_TDT/4] = g_tx_tail;
    mmio_flush();
    /* 实机: 1秒 TSC 超时等待 TX 完成 */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
    while (dkm_rdtsc() < deadline) {
        dma_fence();
        if (g_tx_desc[index].status & E1000_TX_STA_DD) {
                g_stat_tx_ok++;
                DKM_STAT_INC(e1k_tx_packets);
                net_stat_tick();
            return 0;
        }
        __asm__ volatile("pause");
    }
    g_stat_tx_timeout++;
    g_stat_tx_err++;
    raw_log("[e1000] tx timeout\n");
    raw_hex("[e1000] STATUS=", g_mmio[E1000_STATUS/4]);
    raw_hex("[e1000] TCTL=", g_mmio[E1000_TCTL/4]);
    raw_hex("[e1000] TDH=", g_mmio[E1000_TDH/4]);
    raw_hex("[e1000] TDT=", g_mmio[E1000_TDT/4]);
    return -3;
}

static int e1000_rx_poll(void *ctx, void *buffer, u32 capacity, u32 *out_length) {
    (void)ctx;
    if (out_length) *out_length = 0;
    if (!g_rings_ready || !buffer || !out_length) return -1;
    u32 next = (g_rx_tail + 1) % E1000_RX_DESC_COUNT;
    dma_fence();
    if (!(g_rx_desc[next].status & E1000_RX_STA_DD)) return 1;  /* no data */
    u32 len = g_rx_desc[next].length;
    if (len > capacity) {
        g_stat_rx_overflow++;
        g_rx_desc[next].status = 0;
        dma_fence();
        g_rx_tail = next;
        g_mmio[E1000_RDT/4] = g_rx_tail;
        mmio_flush();
        return -2;
    }
    copy_bytes(buffer, g_rx_buf[next], len);
    *out_length = len;
    g_rx_desc[next].status = 0;
    dma_fence();
    g_rx_tail = next;
    g_mmio[E1000_RDT/4] = g_rx_tail;
    mmio_flush();
    g_stat_rx_ok++;
    DKM_STAT_INC(e1k_rx_packets);
    net_stat_tick();
    return 0;
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    /* 实机要求: 先校准 TSC, 再使用基于 CPU 频率的延迟 */
    dkm_tsc_calibrate();
    raw_log("[e1000] driver_init entered\n");
    (void)handle;
    if (!api||!api->log) return -1;
    g_log=api->log;
    dkm_instr_init(api);

    u8 bus, dev, func;
    const struct e1000_device_id *dev_id = 0;
    if (e1000_find(&bus, &dev, &func, &dev_id) != 0) {
        raw_log("[e1000] device not found\n");
        return 0;
    }

    g_device_id = (u16)(pci_read(bus, dev, func, PCI_VENDOR_ID) >> 16);
    g_device_family = dev_id ? dev_id->family : 0;
    raw_log("[e1000] found device: ");
    raw_log(dev_id ? dev_id->name : "unknown");
    raw_log(g_device_family == 1 ? " (e1000e)" : " (classic)");
    raw_log("\n");
    raw_hex("[e1000] PCI bus:dev:func=", ((u64)bus << 16) | ((u64)dev << 8) | func);

    /* 检查 PCI class code 确认是网络设备 (class=0x02) */
    u32 class_reg = pci_read(bus, dev, func, 0x08);
    u8 class_code = (u8)(class_reg >> 24);
    if (class_code != 0x02) {
        raw_log("[e1000] WARNING: PCI class is not Network\n");
    }

    u32 cmd = pci_read(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUSM;
    pci_write(bus, dev, func, PCI_COMMAND, cmd);
    raw_log("[e1000] PCI command enabled\n");

    u32 bar0 = pci_read(bus, dev, func, PCI_BAR0);
    if (bar0 & 1u) {
        raw_log("[e1000] IO BAR unsupported\n");
        return 0;
    }
    g_bar0_phys = (u64)(bar0 & 0xFFFFFFF0ULL);
    if ((bar0 & 0x6u) == 0x4u) {
        u32 bar1 = pci_read(bus, dev, func, PCI_BAR1);
        g_bar0_phys |= ((u64)bar1 << 32);
    }
    raw_hex("[e1000] BAR0 phys=", g_bar0_phys);
    if (!g_bar0_phys) {
        raw_log("[e1000] BAR0 unavailable\n");
        return 0;
    }

    /* 真机 BAR0 可能在 4G 以上（如 I219），需 mm_map_mmio 检查 */
    u64 hhdm = api->hhdm_offset;
    g_mmio = (volatile u32*)(uintptr_t)(hhdm + g_bar0_phys);
    /* 尝试读取 STATUS 寄存器验证映射是否有效 */
    u32 status_val = g_mmio[E1000_STATUS/4];
    (void)status_val;
    raw_log("[e1000] MMIO status read ok\n");

    /* 软件复位，确保 RX/TX/RAL/RAH 从已知状态开始配置。 */
    g_mmio[E1000_CTRL/4] = g_mmio[E1000_CTRL/4] | E1000_CTRL_RST;
    /* 实机: 1秒 TSC 超时等待复位完成 */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
    while (dkm_rdtsc() < deadline) {
        if (!(g_mmio[E1000_CTRL/4] & E1000_CTRL_RST)) break;
        __asm__ volatile("pause");
    }
    if (g_mmio[E1000_CTRL/4] & E1000_CTRL_RST) {
        raw_log("[e1000] WARNING: reset did not complete\n");
    } else {
        raw_log("[e1000] reset done\n");
    }

    /* 等待 EEPROM 完成（实机复位后 EEPROM 状态机需要时间） */
    dkm_delay_ms(1);

    /* 读 MAC 地址：先尝试 RAL/RAH，空时走 EEPROM */
    u32 ral = g_mmio[E1000_RAL/4];
    u32 rah = g_mmio[E1000_RAH/4];
    u8 mac[6];
    mac[0] = (u8)(ral & 0xff);
    mac[1] = (u8)((ral>>8) & 0xff);
    mac[2] = (u8)((ral>>16) & 0xff);
    mac[3] = (u8)((ral>>24) & 0xff);
    mac[4] = (u8)(rah & 0xff);
    mac[5] = (u8)((rah>>8) & 0xff);

    /* RAL/RAH 为空时从 EEPROM 读 MAC */
    if (ral == 0 && (rah & 0xffff) == 0) {
        u16 w0 = e1000_eeprom_read(0);
        u16 w1 = e1000_eeprom_read(1);
        u16 w2 = e1000_eeprom_read(2);
        if (!(w0==0xffff && w1==0xffff && w2==0xffff)) {
            mac[0]=(u8)(w0&0xff); mac[1]=(u8)(w0>>8);
            mac[2]=(u8)(w1&0xff); mac[3]=(u8)(w1>>8);
            mac[4]=(u8)(w2&0xff); mac[5]=(u8)(w2>>8);
            raw_log("[e1000] MAC from EEPROM\n");
        } else {
            raw_log("[e1000] EEPROM read failed (all 0xffff)\n");
        }
    } else {
        raw_log("[e1000] MAC from RAL/RAH\n");
    }

    /* 仍为全 0 或多播位错误时——严格错误策略插桩：MAC 读不到 = 网卡
     * 身份不可知，零降级 panic（E1K-E01），不再用硬编码 MAC 冒充。 */
    if ((mac[0]==0&&mac[1]==0&&mac[2]==0&&mac[3]==0&&mac[4]==0&&mac[5]==0) ||
        (mac[0] & 1)) {
        raw_log("[e1000] MAC read failed (zero/multicast)\n");
        g_log->panic("E1K-E01 MAC READ FAILED");
    }

    raw_mac("[e1000] MAC=", mac);

    /* 写入 RAL/RAH 并置 AV(bit31) 使能接收该单播地址 */
    g_mmio[E1000_RAL/4] = (u32)mac[0] | ((u32)mac[1]<<8) | ((u32)mac[2]<<16) | ((u32)mac[3]<<24);
    g_mmio[E1000_RAH/4] = (u32)mac[4] | ((u32)mac[5]<<8) | (1u<<31);
    dma_fence();

    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    raw_hex("[e1000] IRQ line=", irq_line);

    /* 设置链路：FD + SLU，并允许自动协商 */
    g_mmio[E1000_CTRL/4] = g_mmio[E1000_CTRL/4] | E1000_CTRL_FD | E1000_CTRL_SLU;
    mmio_flush();

    g_mmio[E1000_IMC/4] = 0xffffffffu;
    (void)g_mmio[E1000_ICR/4];
    raw_log("[e1000] interrupts masked\n");

    /* 等待链路就绪（实机 PHY 自动协商可能需要 1-2 秒） */
    int link_rc = e1000_wait_for_link();
    if (link_rc == 0) {
        raw_log("[e1000] link up after auto-negotiation\n");
    } else {
        raw_log("[e1000] link not yet up (will retry during operation)\n");
    }

    /* 打印链路诊断 */
    e1000_log_link_status();

    int ring_rc = e1000_init_rings(api->dma);
    if (ring_rc != 0) {
        raw_log("[e1000] ring init failed\n");
        raw_hex("[e1000] ring_rc=", (u64)(i32)ring_rc);
    }

    /* 根据链路状态设置 netdev flags */
    u32 netdev_flags = 0;
    if (e1000_link_up()) netdev_flags |= DKM_NET_F_LINK_UP;
    if (g_rings_ready) netdev_flags |= DKM_NET_F_TX_READY | DKM_NET_F_RX_READY;

    if (api->net && api->net->register_device) {
        struct dkm_net_device_desc netdev;
        zero_bytes(&netdev, sizeof(netdev));
        netdev.name = "e1000";
        for (int i=0;i<6;i++) netdev.mac[i] = mac[i];
        netdev.flags = netdev_flags;
        netdev.ctx = 0;
        netdev.tx = g_rings_ready ? e1000_tx : 0;
        netdev.rx_poll = g_rings_ready ? e1000_rx_poll : 0;
        int net_index = api->net->register_device(&netdev);
        raw_hex("[e1000] netdev registered, index=", (u64)(i32)net_index);
    } else {
        raw_log("[e1000] net API unavailable\n");
    }

    g_mmio[E1000_IMC/4] = 0xffffffffu;

    if (api->irq_register && irq_line < 16 && irq_line > 0) {
        api->irq_register((u8)irq_line, (void*)(uintptr_t)irq_handler);
        raw_log("[e1000] IRQ registered\n");
    } else {
        raw_log("[e1000] IRQ not registered (polling mode)\n");
    }

    /* 最终诊断快照 */
    e1000_log_link_status();
    raw_log("[e1000] driver ready\n");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    /* 关闭 RX/TX，释放 rings */
    if (g_mmio) {
        g_mmio[E1000_RCTL/4] = 0;
        g_mmio[E1000_TCTL/4] = 0;
        g_mmio[E1000_IMC/4] = 0xffffffffu;
        mmio_flush();
    }
    g_rings_ready = 0;
    g_mmio = 0;
    return 0;
}
