/* DKM virtio-net Driver — PCI virtio network discovery
 * Stage 3 optional network driver, depends on "pci" and "irq", provides "netdev".
 * This first version discovers virtio-net devices and logs PCI capabilities;
 * it does not negotiate features, create virtqueues, or transmit packets.
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

#define DKM_NET_F_LINK_UP (1u << 0)

struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci;
    const void *dma;
    const void *vfs;
    const struct dkm_net_api *net;
    const void *timer;
    const void *drr;
    const void *rsdp_address;
    const void *fb_address;
    u64 fb_width;
    u64 fb_height;
    u64 fb_pitch;
    u16 fb_bpp;
    const void *boot_modules_response;
    int (*irq_register)(u8 irq, void *handler);
    u64 hhdm_offset;
};

struct dkm_driver_handle;

struct dkm_driver_desc {
    u32 magic;
    u16 abi_version;
    u16 desc_size;
    const char *name;
    const char *version;
    const char *vendor;
    u32 driver_class;
    u32 stage;
    u32 flags;
    u32 priority;
    const char *const *depends;
    u32 depends_count;
    const char *const *provides;
    u32 provides_count;
    u64 min_kernel_abi;
    u64 feature_bits;
    u64 reserved0;
    u64 reserved1;
};

static const char *const g_depends[] = { "pci", "irq" };
static const char *const g_provides[] = { "netdev" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "virtio_net",
    .version        = "0.1.0",
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

#define PCI_ADDR       0xCF8
#define PCI_DATA       0xCFC
#define PCI_VENDOR_ID  0x00
#define PCI_COMMAND    0x04
#define PCI_CLASS_REG  0x08
#define PCI_HEADER     0x0E
#define PCI_CAP_PTR    0x34
#define PCI_IRQ_LINE   0x3C
#define PCI_BAR0       0x10
#define PCI_BAR1       0x14
#define PCI_BAR2       0x18
#define PCI_BAR3       0x1C
#define PCI_BAR4       0x20
#define PCI_BAR5       0x24

#define PCI_CMD_IO     (1u << 0)
#define PCI_CMD_MEM    (1u << 1)
#define PCI_CMD_BUSM   (1u << 2)

#define PCI_VENDOR_VIRTIO       0x1AF4u
#define VIRTIO_TRANS_DEVICE_MIN 0x1000u
#define VIRTIO_TRANS_DEVICE_MAX 0x103Fu
#define VIRTIO_MODERN_DEVICE_MIN 0x1040u
#define VIRTIO_MODERN_DEVICE_MAX 0x107Fu
#define VIRTIO_NET_DEVICE_ID    1u

#define VIRTIO_PCI_CAP_COMMON_CFG  1u
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2u
#define VIRTIO_PCI_CAP_ISR_CFG     3u
#define VIRTIO_PCI_CAP_DEVICE_CFG  4u
#define VIRTIO_PCI_CAP_PCI_CFG     5u

static const struct dkm_log_api *g_log;

static __inline__ void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u32 inl(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 addr = (1u << 31)
             | ((u32)bus << 16)
             | ((u32)dev << 11)
             | ((u32)func << 8)
             | ((u32)reg & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 value) {
    u32 addr = (1u << 31)
             | ((u32)bus << 16)
             | ((u32)dev << 11)
             | ((u32)func << 8)
             | ((u32)reg & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, value);
}

static u8 pci_read8(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 v = pci_read(bus, dev, func, reg);
    return (u8)((v >> ((reg & 3) * 8)) & 0xff);
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

static u16 virtio_subsystem_id(u16 device_id) {
    if (device_id >= VIRTIO_TRANS_DEVICE_MIN && device_id <= VIRTIO_TRANS_DEVICE_MAX) {
        return (u16)(device_id - VIRTIO_TRANS_DEVICE_MIN);
    }
    if (device_id >= VIRTIO_MODERN_DEVICE_MIN && device_id <= VIRTIO_MODERN_DEVICE_MAX) {
        return (u16)(device_id - VIRTIO_MODERN_DEVICE_MIN);
    }
    return 0xffff;
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

                u16 virtio_id = virtio_subsystem_id(device);
                u32 class_reg = pci_read(bus, dev, func, PCI_CLASS_REG);
                u8 class_code = (u8)(class_reg >> 24);
                if (virtio_id == VIRTIO_NET_DEVICE_ID && class_code == 0x02) {
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

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

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
    u16 virtio_id = virtio_subsystem_id(device);
    u32 class_reg = pci_read(bus, dev, func, PCI_CLASS_REG);
    u8 revision = (u8)(class_reg & 0xff);
    u8 prog_if = (u8)((class_reg >> 8) & 0xff);
    u8 subclass = (u8)((class_reg >> 16) & 0xff);
    u8 class_code = (u8)((class_reg >> 24) & 0xff);

    log_hex("[virtio_net] vendor=", vendor);
    log_hex("[virtio_net] device=", device);
    log_hex("[virtio_net] virtio_id=", virtio_id);
    log_hex("[virtio_net] class=", class_code);
    log_hex("[virtio_net] subclass=", subclass);
    log_hex("[virtio_net] prog_if=", prog_if);
    log_hex("[virtio_net] revision=", revision);

    u32 command = pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUSM;
    pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[virtio_net] PCI command=", command);

    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[virtio_net] PCI IRQ line=", irq_line);

    log_bars(bus, dev, func);
    log_virtio_caps(bus, dev, func);

    if (api->net && api->net->register_device) {
        struct dkm_net_device_desc netdev;
        netdev.name = "virtio-net";
        for (int i = 0; i < 6; i++) netdev.mac[i] = 0;
        netdev.flags = DKM_NET_F_LINK_UP;
        netdev.ctx = 0;
        netdev.tx = 0;
        netdev.rx_poll = 0;
        int net_index = api->net->register_device(&netdev);
        log_hex("[virtio_net] netdev register rc=", (u64)(i64)net_index);
    } else {
        g_log->warn("[virtio_net] net API unavailable; netdev not registered");
    }

    g_log->info("[virtio_net] discovery only; virtqueues disabled");
    g_log->info("[virtio_net] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
