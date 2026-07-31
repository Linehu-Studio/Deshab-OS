/* DKM PCI Driver — PCI bus enumeration via config space (0xCF8/0xCFC)
 * Stage 0, required, depends on "irq", provides "pci".
 * No IRQ needed for enumeration. Scans all buses/devices/functions.
 *
 * 真机增强:
 * - 多总线扫描（桥接设备下游 bus > 0）
 * - 网卡型号识别（常见 Intel/Realtek 网卡芯片自动标注）
 * - PCI 桥检测和副总线号读取
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

/* PCI 桥配置 (header type 1) */
#define PCI_PRI_BUS     0x18
#define PCI_SEC_BUS     0x19
#define PCI_SUB_BUS     0x1A

#define PCI_CMD_IO     (1u << 0)
#define PCI_CMD_MEM    (1u << 1)
#define PCI_CMD_BUSM   (1u << 2)

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
    .version        = "0.2.0",
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

/* ---- 网卡型号识别 ---- */
struct netchip_id {
    u16 vendor;
    u16 device;
    const char *name;
    u8  driver_hint;  /* 0=e1000, 1=e1000e, 2=r8169, 3=igb, 4=ath9k */
};

static const struct netchip_id g_netchips[] = {
    /* Intel e1000 经典 (8254x) */
    {0x8086, 0x100E, "82540EM",        0},
    {0x8086, 0x1004, "82543GC",        0},
    {0x8086, 0x100F, "82545EM",        0},
    {0x8086, 0x1010, "82546EB",        0},
    {0x8086, 0x1013, "82541EI",        0},
    {0x8086, 0x1019, "82547EI",        0},
    {0x8086, 0x1026, "82545GM",        0},
    {0x8086, 0x1075, "82547GI",        0},
    {0x8086, 0x1076, "82541GI",        0},
    {0x8086, 0x1079, "82546GB",        0},
    {0x8086, 0x108B, "82573E",         0},
    {0x8086, 0x109A, "82573L",         0},

    /* Intel e1000e (8257x/I217/I218/I219) */
    {0x8086, 0x10D3, "82574L",         1},
    {0x8086, 0x10F5, "82578DM",        1},
    {0x8086, 0x10F6, "82578DC",        1},
    {0x8086, 0x1502, "82579V",         1},
    {0x8086, 0x1533, "I217-V",         1},
    {0x8086, 0x1534, "I217-LM",        1},
    {0x8086, 0x153A, "I218-V",         1},
    {0x8086, 0x153B, "I218-LM",        1},
    {0x8086, 0x156F, "I219-V",         1},
    {0x8086, 0x1570, "I219-LM",        1},
    {0x8086, 0x157B, "I219-V-2",       1},
    {0x8086, 0x15D3, "I219-V-3",       1},
    {0x8086, 0x15D8, "I219-LM-3",      1},
    {0x8086, 0x15BD, "I219-V-6",       1},

    /* Intel I210/I211 (igb) */
    {0x8086, 0x1537, "I210",           3},
    {0x8086, 0x1536, "I211-AT",        3},

    /* Intel 82575/82576/82580 (igb) */
    {0x8086, 0x10A7, "82575EB",        3},
    {0x8086, 0x10C9, "82576",          3},
    {0x8086, 0x150D, "82580",          3},
    {0x8086, 0x1526, "82576-2",        3},

    /* Realtek RTL8111/8168/8125 (r8169) — 常见桌面/笔记本有线网卡 */
    {0x10EC, 0x8167, "RTL8111/8168B",  2},
    {0x10EC, 0x8168, "RTL8111/8168",   2},
    {0x10EC, 0x8169, "RTL8169",        2},
    {0x10EC, 0x8125, "RTL8125",        2},
    {0x10EC, 0x3000, "RTL8169S",       2},
    {0x10EC, 0x8136, "RTL8101E",       2},

    /* Qualcomm Atheros WiFi (ath9k) */
    {0x168C, 0x0023, "AR5416",         4},
    {0x168C, 0x0024, "AR5418",         4},
    {0x168C, 0x0027, "AR9160",         4},
    {0x168C, 0x0029, "AR9280",         4},
    {0x168C, 0x002A, "AR9285",         4},
    {0x168C, 0x002B, "AR9287",         4},
    {0x168C, 0x002D, "AR9287-2",       4},
    {0x168C, 0x002E, "AR9287-3",       4},
    {0x168C, 0x0030, "AR9300",         4},
    {0x168C, 0x0032, "AR9485",         4},
    {0x168C, 0x0034, "AR9462",         4},
    {0x168C, 0x0036, "AR9485-2",       4},
    {0x168C, 0x0037, "AR1111",         4},

    /* Broadcom WiFi — 目前无驱动，标注识别 */
    {0x14E4, 0x4315, "BCM4312-WiFi",   0xFF},
    {0x14E4, 0x4365, "BCM4365-WiFi",   0xFF},
    {0x14E4, 0x43A0, "BCM4360-WiFi",   0xFF},
    {0x14E4, 0x43C3, "BCM4366-WiFi",   0xFF},

    /* Qualcomm WiFi — 无驱动 */
    {0x168C, 0x003E, "QCA6174-WiFi",   0xFF},
    {0x168C, 0x0042, "QCA6390-WiFi",   0xFF},

    /* Intel WiFi — 无驱动 */
    {0x8086, 0x24FD, "Intel-8265-WiFi", 0xFF},
    {0x8086, 0x2526, "Intel-AX200-WiFi",0xFF},
    {0x8086, 0x2725, "Intel-AX210-WiFi",0xFF},
    {0x8086, 0x51F0, "Intel-AX211-WiFi",0xFF},
    {0x8086, 0x7A70, "Intel-BE200-WiFi",0xFF},

    {0, 0, 0, 0}  /* 终止符 */
};

static const char *netchip_name(u16 vendor, u16 device) {
    for (int i = 0; g_netchips[i].name != 0; i++) {
        if (g_netchips[i].vendor == vendor && g_netchips[i].device == device)
            return g_netchips[i].name;
    }
    return 0;
}

static u8 netchip_driver(u16 vendor, u16 device) {
    for (int i = 0; g_netchips[i].name != 0; i++) {
        if (g_netchips[i].vendor == vendor && g_netchips[i].device == device)
            return g_netchips[i].driver_hint;
    }
    return 0xFF;
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

/* PCI subclass names for Network class */
static const char *net_subclass_name(u8 sub) {
    switch (sub) {
        case 0x00: return "Ethernet";
        case 0x01: return "TokenRing";
        case 0x02: return "FDDI";
        case 0x03: return "ATM";
        case 0x04: return "ISDN";
        case 0x05: return "WorldFIP";
        case 0x06: return "PICMG";
        case 0x80: return "Other";
        default:   return "Net-?";
    }
}

/* ---------------------------------------------------------------
 * Enumeration
 * --------------------------------------------------------------- */
static u32 g_device_count;

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
            u8 prog_if    = (u8)((cc >> 8) & 0xff);

            g_device_count++;
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

            /* 网卡自动识别与驱动匹配提示 */
            if (class_code == 0x02) {
                const char *chip_name = netchip_name(vendor2, device);
                u8 drv = netchip_driver(vendor2, device);
                g_log->info("[pci] NetChip");
                if (chip_name) {
                    g_log->info(chip_name);
                } else {
                    g_log->info("unknown");
                }
                g_log->info("[pci] NetSub");
                g_log->info(net_subclass_name(subclass));
                switch (drv) {
                    case 0: g_log->info("[pci] -> driver: e1000"); break;
                    case 1: g_log->info("[pci] -> driver: e1000 (e1000e compat)"); break;
                    case 2: g_log->info("[pci] -> driver: r8169 (NOT YET IMPLEMENTED)"); break;
                    case 3: g_log->info("[pci] -> driver: igb (e1000 compat)"); break;
                    case 4: g_log->info("[pci] -> driver: ath9k"); break;
                    case 0xFF: g_log->info("[pci] -> driver: NONE (unsupported)"); break;
                    default: break;
                }
            }

            /* PCI-PCI 桥检测: 递归扫描下游总线 */
            if (class_code == 0x06 && (subclass == 0x04 || subclass == 0x09)) {
                u8 sec_bus = (u8)pci_cfg_read(bus, dev, func, PCI_SEC_BUS);
                if (sec_bus > bus && sec_bus < 255) {
                    g_log->info("[pci] bridge -> secondary bus");
                    log_hex("[pci] sec_bus=", sec_bus);
                    pci_scan_bus(sec_bus);
                }
            }
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

    g_device_count = 0;
    g_log->info("[pci] scanning PCI bus");
    pci_scan_bus(0);
    log_hex("[pci] total devices=", g_device_count);
    g_log->info("[pci] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
