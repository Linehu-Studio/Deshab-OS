/* DKM NVMe Driver — PCI NVMe discovery and controller register enumeration
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 * This first version only discovers NVMe controllers and reads BAR0 registers;
 * it does not enable queues or submit admin commands.
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
    const void *net;
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
static const char *const g_provides[] = { "block" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "nvme",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 6,   /* DKM_CLASS_STORAGE */
    .stage          = 1,
    .flags          = 0,   /* optional */
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
#define PCI_BAR0       0x10
#define PCI_BAR1       0x14
#define PCI_IRQ_LINE   0x3C

#define PCI_CMD_MEM    (1u << 1)
#define PCI_CMD_BUSM   (1u << 2)

#define NVME_CLASS_STORAGE 0x01
#define NVME_SUBCLASS_NVM  0x08
#define NVME_PROGIF_NVME   0x02

#define NVME_CAP      0x00
#define NVME_VS       0x08
#define NVME_INTMS    0x0C
#define NVME_INTMC    0x10
#define NVME_CC       0x14
#define NVME_CSTS     0x1C
#define NVME_NSSR     0x20
#define NVME_AQA      0x24
#define NVME_ASQ      0x28
#define NVME_ACQ      0x30
#define NVME_CMBLOC   0x38
#define NVME_CMBSZ    0x3C

static const struct dkm_log_api *g_log;
static volatile u8 *g_regs;

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

static u32 mmio_read32(u32 off) {
    return *(volatile u32 *)(void *)(g_regs + off);
}

static u64 mmio_read64(u32 off) {
    u32 lo = mmio_read32(off);
    u32 hi = mmio_read32(off + 4);
    return ((u64)hi << 32) | lo;
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

static int nvme_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
    for (u8 bus = 0; bus < 16; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = pci_read(bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;

            u8 header = (u8)(pci_read(bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;
            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = pci_read(bus, dev, func, PCI_VENDOR_ID);
                if ((vd2 & 0xffff) == 0xffff) continue;

                u32 class_reg = pci_read(bus, dev, func, PCI_CLASS_REG);
                u8 prog_if = (u8)((class_reg >> 8) & 0xff);
                u8 subclass = (u8)((class_reg >> 16) & 0xff);
                u8 class_code = (u8)((class_reg >> 24) & 0xff);

                if (class_code == NVME_CLASS_STORAGE &&
                    subclass == NVME_SUBCLASS_NVM &&
                    prog_if == NVME_PROGIF_NVME) {
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

static void log_nvme_version(u32 vs) {
    log_hex("[nvme] version major=", (vs >> 16) & 0xffff);
    log_hex("[nvme] version minor=", (vs >> 8) & 0xff);
    log_hex("[nvme] version tertiary=", vs & 0xff);
}

static void log_cap_fields(u64 cap) {
    log_hex("[nvme] CAP.MQES=", cap & 0xffff);
    log_hex("[nvme] CAP.CQR=", (cap >> 16) & 1);
    log_hex("[nvme] CAP.AMS=", (cap >> 17) & 3);
    log_hex("[nvme] CAP.TO=", (cap >> 24) & 0xff);
    log_hex("[nvme] CAP.DSTRD=", (cap >> 32) & 0xf);
    log_hex("[nvme] CAP.NSSRS=", (cap >> 36) & 1);
    log_hex("[nvme] CAP.CSS=", (cap >> 37) & 0xff);
    log_hex("[nvme] CAP.MPSMIN=", (cap >> 48) & 0xf);
    log_hex("[nvme] CAP.MPSMAX=", (cap >> 52) & 0xf);
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    g_log->info("[nvme] init begin");

    u8 bus = 0;
    u8 dev = 0;
    u8 func = 0;
    if (nvme_find(&bus, &dev, &func) != 0) {
        g_log->warn("[nvme] NVMe controller not found");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    g_log->info("[nvme] NVMe controller found");
    log_hex("[nvme] bus=", bus);
    log_hex("[nvme] dev=", dev);
    log_hex("[nvme] func=", func);

    u32 vd = pci_read(bus, dev, func, PCI_VENDOR_ID);
    log_hex("[nvme] vendor=", vd & 0xffff);
    log_hex("[nvme] device=", vd >> 16);

    u32 command = pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_MEM | PCI_CMD_BUSM;
    pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[nvme] PCI command=", command);

    u32 bar0_lo = pci_read(bus, dev, func, PCI_BAR0);
    u32 bar1_hi = pci_read(bus, dev, func, PCI_BAR1);
    u64 bar_phys;
    if (bar0_lo & 1u) {
        g_log->warn("[nvme] BAR0 is IO space; unsupported");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    if ((bar0_lo & 0x6u) == 0x4u) {
        bar_phys = ((u64)bar1_hi << 32) | (u64)(bar0_lo & 0xFFFFFFF0u);
    } else {
        bar_phys = (u64)(bar0_lo & 0xFFFFFFF0u);
    }

    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[nvme] BAR0 phys=", bar_phys);
    log_hex("[nvme] PCI IRQ line=", irq_line);

    if (!api->hhdm_offset || !bar_phys) {
        g_log->warn("[nvme] missing HHDM or BAR0; skip MMIO enumeration");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    if (bar_phys >= 0x100000000ULL) {
        g_log->warn("[nvme] BAR0 is above 4G; current HHDM MMIO map cannot safely read it");
        g_log->info("[nvme] discovery only; MMIO enumeration skipped");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    g_regs = (volatile u8 *)(uintptr_t)(api->hhdm_offset + bar_phys);
    log_hex("[nvme] HHDM offset=", api->hhdm_offset);

    u64 cap = mmio_read64(NVME_CAP);
    u32 vs = mmio_read32(NVME_VS);
    u32 intms = mmio_read32(NVME_INTMS);
    u32 intmc = mmio_read32(NVME_INTMC);
    u32 cc = mmio_read32(NVME_CC);
    u32 csts = mmio_read32(NVME_CSTS);
    u32 nssr = mmio_read32(NVME_NSSR);
    u32 aqa = mmio_read32(NVME_AQA);
    u64 asq = mmio_read64(NVME_ASQ);
    u64 acq = mmio_read64(NVME_ACQ);
    u32 cmbloc = mmio_read32(NVME_CMBLOC);
    u32 cmbsz = mmio_read32(NVME_CMBSZ);

    log_hex("[nvme] CAP=", cap);
    log_cap_fields(cap);
    log_hex("[nvme] VS=", vs);
    log_nvme_version(vs);
    log_hex("[nvme] INTMS=", intms);
    log_hex("[nvme] INTMC=", intmc);
    log_hex("[nvme] CC=", cc);
    log_hex("[nvme] CSTS=", csts);
    log_hex("[nvme] NSSR=", nssr);
    log_hex("[nvme] AQA=", aqa);
    log_hex("[nvme] ASQ=", asq);
    log_hex("[nvme] ACQ=", acq);
    log_hex("[nvme] CMBLOC=", cmbloc);
    log_hex("[nvme] CMBSZ=", cmbsz);

    if (cc & 1u) {
        g_log->warn("[nvme] controller already enabled; leaving state unchanged");
    } else {
        g_log->info("[nvme] controller disabled; queues not configured");
    }

    g_log->info("[nvme] discovery only; admin queues disabled");
    g_log->info("[nvme] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
