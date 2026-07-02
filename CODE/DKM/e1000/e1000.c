/* DKM e1000 Driver — Intel 8254x Gigabit Ethernet
 * Stage 3, optional, depends on "pci" and "irq", provides "netdev".
 * Finds e1000 via PCI, maps MMIO, reads MAC address.
 */

#include <stdint.h>

#define DKM_DRIVER_MAGIC 0x444B4D31u
#define DKM_ABI_VERSION  1u

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;
#define NULL ((void *)0)

struct dkm_log_api {
    void (*info)(const char *msg);
    void (*warn)(const char *msg);
    void (*error)(const char *msg);
    void (*panic)(const char *msg);
};

struct dkm_net_device_desc {
    const char *name;
    u8 mac[6];
    u32 flags;
    void *ctx;
    int (*tx)(void *ctx, const void *packet, u32 length);
    int (*rx_poll)(void *ctx, void *buffer, u32 capacity, u32 *out_length);
};

struct dkm_net_api {
    int (*register_device)(const struct dkm_net_device_desc *desc);
    u32 (*device_count)(void);
    int (*device_info)(u32 index, void *out);
};

struct dkm_dma_buffer {
    void *virt;
    u64 phys;
    u64 size;
};

struct dkm_dma_api {
    int (*alloc_pages)(u64 page_count, u64 alignment, u64 max_phys, struct dkm_dma_buffer *out);
};

#define DKM_NET_F_LINK_UP  (1u << 0)
#define DKM_NET_F_TX_READY (1u << 1)
#define DKM_NET_F_RX_READY (1u << 2)

struct dkm_kernel_api {
    u32 version; u32 size; u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem,*utsm,*irq_api,*pci_api;
    const struct dkm_dma_api *dma;
    const void *vfs_api;
    const struct dkm_net_api *net;
    const void *timer,*drr;
    const void *rsdp_address,*fb_address;
    u64 fb_width,fb_height,fb_pitch; u16 fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(u8 irq, void *handler);
    u64 hhdm_offset;
};

struct dkm_driver_handle;
struct dkm_driver_desc {
    u32 magic; u16 abi_version; u16 desc_size;
    const char *name,*version,*vendor;
    u32 driver_class,stage,flags,priority;
    const char *const *depends; u32 depends_count;
    const char *const *provides; u32 provides_count;
    u64 min_kernel_abi,feature_bits,reserved0,reserved1;
};

static const char *const g_depends[] = {"pci","irq"};
static const char *const g_provides[] = {"netdev"};
__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC,.abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="e1000",.version="0.1.0",.vendor="Deshab",
    .driver_class=9,.stage=3,.flags=0,.priority=0,
    .depends=g_depends,.depends_count=2,
    .provides=g_provides,.provides_count=1,.min_kernel_abi=1,
};

/* PCI config ports */
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND   0x04
#define PCI_BAR0 0x10
#define PCI_IRQ_LINE 0x3C

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

#define E1000_RX_DESC_COUNT 8u
#define E1000_TX_DESC_COUNT 8u
#define E1000_RX_BUF_SIZE   2048u
#define E1000_TX_BUF_SIZE   2048u
#define E1000_POOL_BYTES(count, size) ((u32)((count) * (size)))
#define E1000_POOL_PAGES(bytes) (((bytes) + 4095u) / 4096u)

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
#define E1000_CTRL_FD       (1u << 0)
#define E1000_CTRL_SLU      (1u << 6)

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

static __inline__ void outl(u16 p, u32 v) { __asm__("outl %0,%1"::"a"(v),"Nd"(p)); }
static __inline__ u32 inl(u16 p) { u32 v; __asm__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static u8 inb(u16 p) { u8 v; __asm__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static void outb(u16 p, u8 v) { __asm__("outb %0,%1"::"a"(v),"Nd"(p)); }

static void raw_log(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            for (u32 i=0;i<100000;i++) if (inb(0x3F8+5)&0x20) break;
            outb(0x3F8, '\r');
        }
        for (u32 i=0;i<100000;i++) if (inb(0x3F8+5)&0x20) break;
        outb(0x3F8, (u8)c);
    }
}

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 a = (1u<<31)|((u32)bus<<16)|((u32)dev<<11)|((u32)func<<8)|((u32)reg&0xFC);
    outl(PCI_ADDR,a); return inl(PCI_DATA);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 v) {
    u32 a = (1u<<31)|((u32)bus<<16)|((u32)dev<<11)|((u32)func<<8)|((u32)reg&0xFC);
    outl(PCI_ADDR,a); outl(PCI_DATA,v);
}

static int e1000_find(u8 *bus, u8 *dev) {
    for (u8 d=0; d<32; d++) {
        u32 vd = pci_read(0,d,0,PCI_VENDOR_ID);
        if ((vd&0xffff)==0xffff) continue;
        if ((vd&0xffff)!=0x8086) continue;
        u16 did = (u16)(vd>>16);
        /* 82540EM(0x100E) / 82545 等经典 e1000，以及 82574L(0x10D3) e1000e */
        if (did==0x100E || did==0x10D3 || did==0x1004 || did==0x100F ||
            did==0x10D3 || did==0x153A || did==0x1533) {
            *bus=0; *dev=d; return 0;
        }
    }
    return -1;
}

/* 读取 EEPROM 一个 16-bit 字（经典 e1000 EERD 接口）。失败返回 0xffff。 */
static u16 e1000_eeprom_read(u16 addr) {
    /* EERD: bit0=START, addr<<8, 完成后 bit4=DONE, data 在高 16 位 */
    g_mmio[E1000_EERD/4] = ((u32)addr << 8) | 1u;
    for (u32 i=0;i<100000;i++) {
        u32 v = g_mmio[E1000_EERD/4];
        if (v & (1u<<4)) return (u16)(v >> 16);
    }
    return 0xffff;
}

static int irq_handler(u8 irq) {
    (void)irq;
    if (!g_mmio) return 0;
    u32 icr = g_mmio[E1000_ICR/4];
    if (icr) {
        g_mmio[E1000_ICR/4] = icr; /* clear */
    }
    return 0;
}

static void copy_bytes(void *dst, const void *src, u32 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i=0;i<len;i++) d[i]=s[i];
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
        g_rx_desc[i].addr = b.phys;
        g_rx_desc[i].status = 0;
    }

    for (u32 i=0;i<E1000_TX_DESC_COUNT;i++) {
        struct dkm_dma_buffer b;
        if (dma->alloc_pages(1, 16, 0x100000000ULL, &b) != 0) return -5;
        g_tx_buf[i] = (u8 *)b.virt;
        g_tx_buf_phys[i] = b.phys;
        g_tx_desc[i].addr = b.phys;
        g_tx_desc[i].status = E1000_TX_STA_DD;
    }
    raw_log("[e1000.raw] ring/buffers allocated\n");

    g_mmio[E1000_RCTL/4] = 0;
    g_mmio[E1000_TCTL/4] = 0;

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
    g_mmio[E1000_RCTL/4] = E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048;
    g_mmio[E1000_TCTL/4] = E1000_TCTL_EN | E1000_TCTL_PSP | (0x0Fu << E1000_TCTL_CT_SHIFT) | (0x40u << E1000_TCTL_COLD_SHIFT);

    g_rings_ready = 1;
    raw_log("[e1000.raw] RX/TX rings initialized\n");
    return 0;
}

static int e1000_tx(void *ctx, const void *packet, u32 length) {
    (void)ctx;
    raw_log("[e1000.raw] tx enter\n");
    if (!g_rings_ready || !packet || length == 0 || length > E1000_RX_BUF_SIZE) return -1;
    u32 index = g_tx_tail;
    if (!(g_tx_desc[index].status & E1000_TX_STA_DD)) return -2;
    copy_bytes(g_tx_buf[index], packet, length);
    raw_log("[e1000.raw] tx copied\n");
    g_tx_desc[index].length = (u16)length;
    g_tx_desc[index].cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    g_tx_desc[index].status = 0;
    g_tx_tail = (index + 1) % E1000_TX_DESC_COUNT;
    g_mmio[E1000_TDT/4] = g_tx_tail;
    raw_log("[e1000.raw] tx tdt written\n");
    return 0;
}

static int e1000_rx_poll(void *ctx, void *buffer, u32 capacity, u32 *out_length) {
    (void)ctx;
    if (!g_rings_ready || !buffer || !out_length) return -1;
    u32 next = (g_rx_tail + 1) % E1000_RX_DESC_COUNT;
    if (!(g_rx_desc[next].status & E1000_RX_STA_DD)) return 1;
    u32 len = g_rx_desc[next].length;
    if (len > capacity) return -2;
    copy_bytes(buffer, g_rx_buf[next], len);
    *out_length = len;
    g_rx_desc[next].status = 0;
    g_rx_tail = next;
    g_mmio[E1000_RDT/4] = g_rx_tail;
    return 0;
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    raw_log("[e1000.raw] driver_init entered\n");
    (void)handle;
    if (!api||!api->log) return -1;
    g_log=api->log;

    raw_log("[e1000.raw] before find\n");
    u8 bus, dev;
    if (e1000_find(&bus,&dev)!=0) {
        raw_log("[e1000.raw] device not found\n");
        return 0;
    }

    raw_log("[e1000.raw] found device\n");

    u32 cmd = pci_read(bus, dev, 0, PCI_COMMAND);
    cmd |= 0x0007u; /* IO Space | Memory Space | Bus Master */
    pci_write(bus, dev, 0, PCI_COMMAND, cmd);
    raw_log("[e1000.raw] PCI command enabled\n");

    g_bar0_phys = (u64)pci_read(bus,dev,0,PCI_BAR0) & 0xFFFFFFF0ULL;
    if (!g_bar0_phys) {
        raw_log("[e1000.raw] BAR0 unavailable\n");
        return 0;
    }
    raw_log("[e1000.raw] BAR0 ok\n");

    u64 hhdm = api->hhdm_offset;
    g_mmio = (volatile u32*)(uintptr_t)(hhdm + g_bar0_phys);
    (void)g_mmio[E1000_STATUS/4];
    raw_log("[e1000.raw] MMIO status read ok\n");

    /* 软件复位，确保 RX/TX/RAL/RAH 从已知状态开始配置。 */
    g_mmio[E1000_CTRL/4] = g_mmio[E1000_CTRL/4] | (1u << 26);
    for (u32 wait=0; wait<1000000; wait++) {
        if (!(g_mmio[E1000_CTRL/4] & (1u << 26))) break;
    }
    raw_log("[e1000.raw] reset done\n");

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
        }
    }

    /* 仍为全 0 时使用与虚拟机配置一致的回退 MAC，保证收发地址匹配 */
    if (mac[0]==0&&mac[1]==0&&mac[2]==0&&mac[3]==0&&mac[4]==0&&mac[5]==0) {
        mac[0]=0x52; mac[1]=0x54; mac[2]=0x00; mac[3]=0x12; mac[4]=0x34; mac[5]=0x56;
        raw_log("[e1000.raw] using fallback MAC 52:54:00:12:34:56\n");
    }

    /* 写入 RAL/RAH 并置 AV(bit31) 使能接收该单播地址 */
    g_mmio[E1000_RAL/4] = (u32)mac[0] | ((u32)mac[1]<<8) | ((u32)mac[2]<<16) | ((u32)mac[3]<<24);
    g_mmio[E1000_RAH/4] = (u32)mac[4] | ((u32)mac[5]<<8) | (1u<<31);
    raw_log("[e1000.raw] MAC read ok\n");

    u32 irq_line = pci_read(bus,dev,0,PCI_IRQ_LINE) & 0xff;

    g_mmio[E1000_CTRL/4] = g_mmio[E1000_CTRL/4] | E1000_CTRL_FD | E1000_CTRL_SLU;
    raw_log("[e1000.raw] link setup\n");

    g_mmio[E1000_IMC/4] = 0xffffffffu;
    (void)g_mmio[E1000_ICR/4];
    raw_log("[e1000.raw] interrupts masked\n");

    int ring_rc = e1000_init_rings(api->dma);
    if (ring_rc != 0) raw_log("[e1000.raw] ring init failed\n");

    if (api->net && api->net->register_device) {
        struct dkm_net_device_desc netdev;
        netdev.name = "e1000";
        for (int i=0;i<6;i++) netdev.mac[i] = mac[i];
        netdev.flags = DKM_NET_F_LINK_UP;
        if (g_rings_ready) netdev.flags |= DKM_NET_F_TX_READY | DKM_NET_F_RX_READY;
        netdev.ctx = 0;
        netdev.tx = g_rings_ready ? e1000_tx : 0;
        netdev.rx_poll = g_rings_ready ? e1000_rx_poll : 0;
        int net_index = api->net->register_device(&netdev);
        (void)net_index;
        raw_log("[e1000.raw] netdev registered\n");
    } else {
        raw_log("[e1000.raw] net API unavailable\n");
    }

    g_mmio[E1000_IMC/4] = 0xffffffffu;

    if (api->irq_register && irq_line < 16) {
        api->irq_register((u8)irq_line, (void*)(uintptr_t)irq_handler);
        raw_log("[e1000.raw] IRQ registered\n");
    }

    raw_log("[e1000.raw] driver ready\n");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle; return 0;
}
