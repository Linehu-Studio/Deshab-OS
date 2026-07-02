/* DKM PCI Driver — PCI bus enumeration via config space (0xCF8/0xCFC)
 * Stage 0, required, depends on "irq", provides "pci".
 * No IRQ needed for enumeration. Scans all buses/devices/functions.
 */

#include <stdint.h>

/* ---------------------------------------------------------------
 * DKM ABI types
 * --------------------------------------------------------------- */
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

struct dkm_kernel_api {
    u32 version;
    u32 size;
    u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem;
    const void *utsm;
    const void *irq;
    const void *pci_api;
    const void *dma;
    const void *vfs;
    const void *net;
    const void *timer;
    const void *drr;
    const void *rsdp_address;
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

/* ---------------------------------------------------------------
 * PCI constants
 * --------------------------------------------------------------- */
#define PCI_CFG_ADDR  0xCF8
#define PCI_CFG_DATA  0xCFC

#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_COMMAND     0x04
#define PCI_STATUS      0x06
#define PCI_CLASS       0x0B   /* class code */
#define PCI_SUBCLASS    0x0A   /* subclass */
#define PCI_PROG_IF     0x09   /* prog IF */
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_depends[] = { "irq" };
static const char *const g_provides[] = { "pci" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "pci",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 2,   /* DKM_CLASS_BUS */
    .stage          = 0,
    .flags          = 1,   /* DKM_F_REQUIRED */
    .priority       = 0,
    .depends        = g_depends,
    .depends_count  = 1,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

/* ---------------------------------------------------------------
 * I/O helpers
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;

static __inline__ void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u32 inl(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ---------------------------------------------------------------
 * PCI config access
 * --------------------------------------------------------------- */
static u32 pci_cfg_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 addr = (u32)((1u << 31)
                   | ((u32)bus   << 16)
                   | ((u32)dev   << 11)
                   | ((u32)func  << 8)
                   | ((u32)reg & 0xFC));
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

static void pci_cfg_write(u8 bus, u8 dev, u8 func, u8 reg, u32 val) {
    u32 addr = (u32)((1u << 31)
                   | ((u32)bus   << 16)
                   | ((u32)dev   << 11)
                   | ((u32)func  << 8)
                   | ((u32)reg & 0xFC));
    outl(PCI_CFG_ADDR, addr);
    outl(PCI_CFG_DATA, val);
}

/* ---------------------------------------------------------------
 * Log helpers
 * --------------------------------------------------------------- */
static void log_hex(const char *prefix, u64 val) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    u32 pos = 0;
    buf[pos++] = '0'; buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static const char *pci_class_name(u8 base, u8 sub) {
    switch (base) {
        case 0x00: return "Legacy";
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "SimpleComm";
        case 0x08: return "BasePeriph";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "CPU";
        case 0x0C: return "SerialBus";
        case 0x0D: return "Wireless";
        case 0x0E: return "IntelligentIO";
        case 0x0F: return "Satellite";
        case 0x10: return "Crypto";
        case 0x11: return "SignalProc";
        case 0x12: return "Accelerator";
        default:   return "Unknown";
    }
    (void)sub;
}

/* ---------------------------------------------------------------
 * Enumeration
 * --------------------------------------------------------------- */
static void pci_scan_bus(u8 bus) {
    for (u8 dev = 0; dev < 32; dev++) {
        u32 vid_dev = pci_cfg_read(bus, dev, 0, PCI_VENDOR_ID);
        u16 vendor = (u16)(vid_dev & 0xffff);
        if (vendor == 0xffff) continue;

        u8 header_type = (u8)(pci_cfg_read(bus, dev, 0, PCI_HEADER_TYPE) >> 16);
        int multi_func = (header_type & 0x80) != 0;

        u8 func_count = multi_func ? 8 : 1;
        for (u8 func = 0; func < func_count; func++) {
            u32 vid_dev2 = pci_cfg_read(bus, dev, func, PCI_VENDOR_ID);
            u16 vendor2 = (u16)(vid_dev2 & 0xffff);
            if (vendor2 == 0xffff) continue;

            u16 device = (u16)(vid_dev2 >> 16);
            u32 cc = pci_cfg_read(bus, dev, func, PCI_CLASS);
            u8 class_code = (u8)(cc >> 24);
            u8 subclass   = (u8)((cc >> 16) & 0xff);

            g_log->info("[pci]");
            log_hex("[pci] bus=", bus);
            log_hex("[pci] dev=", dev);
            log_hex("[pci] func=", func);
            log_hex("[pci] vendor=", vendor2);
            log_hex("[pci] device=", device);
            g_log->info("[pci] class");
            g_log->info(pci_class_name(class_code, subclass));
            log_hex("[pci] class_hex=", class_code);
            log_hex("[pci] subclass=", subclass);
        }
    }
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    g_log->info("[pci] scanning PCI bus");
    pci_scan_bus(0);
    g_log->info("[pci] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
