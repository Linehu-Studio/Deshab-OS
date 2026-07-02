/* DKM AHCI Driver — PCI AHCI discovery and ABAR read-only enumeration
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 * This first version does not issue DMA commands; it only discovers the HBA
 * and ports so later block I/O can be added safely.
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

struct dkm_dma_buffer {
    void *virt;
    u64 phys;
    u64 size;
};

struct dkm_dma_api {
    int (*alloc_pages)(u64 page_count, u64 alignment, u64 max_phys, struct dkm_dma_buffer *out);
};

struct dkm_block_device_desc {
    const char *name;
    u64 sector_size;
    u64 sector_count;
    void *ctx;
    int (*read)(void *ctx, u64 lba, u32 count, void *buffer);
};

struct dkm_block_api {
    int (*register_device)(const struct dkm_block_device_desc *desc);
    u32 (*device_count)(void);
    int (*read)(u32 index, u64 lba, u32 count, void *buffer);
    u64 (*sector_size)(u32 index);
    const char *(*device_name)(u32 index);
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
    const struct dkm_dma_api *dma;
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
    const struct dkm_block_api *block;
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
    .name           = "ahci",
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
#define PCI_PROG_IF    0x09
#define PCI_SUBCLASS   0x0A
#define PCI_CLASS      0x0B
#define PCI_HEADER     0x0E
#define PCI_BAR5       0x24
#define PCI_IRQ_LINE   0x3C

#define PCI_CMD_IO     (1u << 0)
#define PCI_CMD_MEM    (1u << 1)
#define PCI_CMD_BUSM   (1u << 2)

#define AHCI_CLASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA 0x06
#define AHCI_PROGIF_AHCI   0x01

#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_IS      0x08
#define HBA_PI      0x0C
#define HBA_VS      0x10
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28
#define HBA_PORT_BASE 0x100
#define HBA_PORT_SIZE 0x80

#define PxCLB       0x00
#define PxFB        0x08
#define PxIS        0x10
#define PxIE        0x14
#define PxCMD       0x18
#define PxTFD       0x20
#define PxSIG       0x24
#define PxSSTS      0x28
#define PxSCTL      0x2C
#define PxSERR      0x30
#define PxSACT      0x34
#define PxCI        0x38
#define PxSNTF      0x3C
#define PxFBS       0x40

#define AHCI_CMD_ST    (1u << 0)
#define AHCI_CMD_FRE   (1u << 4)
#define AHCI_CMD_FR    (1u << 14)
#define AHCI_CMD_CR    (1u << 15)
#define AHCI_CMD_ICC_ACTIVE (1u << 28)

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1

typedef struct __attribute__((packed)) ahci_cmd_header {
    u16 flags;
    u16 prdtl;
    u32 prdbc;
    u32 ctba;
    u32 ctbau;
    u32 reserved[4];
} ahci_cmd_header;

typedef struct __attribute__((packed)) ahci_prdt_entry {
    u32 dba;
    u32 dbau;
    u32 reserved;
    u32 dbc_i;
} ahci_prdt_entry;

typedef struct __attribute__((packed)) ahci_cmd_table {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];
    ahci_prdt_entry prdt[1];
} ahci_cmd_table;

static const struct dkm_log_api *g_log;
static volatile u32 *g_abar;
static u32 g_ready_port = 0xffffffffu;
static ahci_cmd_header *g_cmd_header;
static struct dkm_dma_buffer g_cmd_table;
static struct dkm_dma_buffer g_data;

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

static void ahci_zero(void *ptr, u32 len) {
    u8 *p = (u8 *)ptr;
    for (u32 i = 0; i < len; i++) p[i] = 0;
}

static int ahci_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
    for (u8 bus = 0; bus < 8; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = pci_read(bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;

            u8 header = (u8)(pci_read(bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;
            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = pci_read(bus, dev, func, PCI_VENDOR_ID);
                if ((vd2 & 0xffff) == 0xffff) continue;

                u32 class_reg = pci_read(bus, dev, func, 0x08);
                u8 prog_if = (u8)((class_reg >> 8) & 0xff);
                u8 subclass = (u8)((class_reg >> 16) & 0xff);
                u8 class_code = (u8)((class_reg >> 24) & 0xff);

                if (class_code == AHCI_CLASS_STORAGE &&
                    subclass == AHCI_SUBCLASS_SATA &&
                    prog_if == AHCI_PROGIF_AHCI) {
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

static const char *sata_sig_name(u32 sig) {
    if (sig == 0x00000101u) return "SATA drive";
    if (sig == 0xEB140101u) return "ATAPI device";
    if (sig == 0xC33C0101u) return "Enclosure mgmt";
    if (sig == 0x96690101u) return "Port multiplier";
    if (sig == 0x00000000u) return "empty/unknown";
    return "unknown signature";
}

static void ahci_log_port(u32 port) {
    if (!g_abar || port >= 32) {
        return;
    }
    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    u32 ssts = g_abar[(base + PxSSTS) / 4];
    u32 sig  = g_abar[(base + PxSIG) / 4];
    u32 cmd  = g_abar[(base + PxCMD) / 4];
    u32 tfd  = g_abar[(base + PxTFD) / 4];
    u32 serr = g_abar[(base + PxSERR) / 4];
    u32 ci   = g_abar[(base + PxCI) / 4];
    u32 sact = g_abar[(base + PxSACT) / 4];

    u32 det = ssts & 0xf;
    u32 spd = (ssts >> 4) & 0xf;
    u32 ipm = (ssts >> 8) & 0xf;

    log_hex("[ahci] port=", port);
    log_hex("[ahci]   SSTS=", ssts);
    log_hex("[ahci]   DET=", det);
    log_hex("[ahci]   SPD=", spd);
    log_hex("[ahci]   IPM=", ipm);
    log_hex("[ahci]   SIG=", sig);
    g_log->info("[ahci]   type");
    g_log->info(sata_sig_name(sig));
    log_hex("[ahci]   CMD=", cmd);
    log_hex("[ahci]   TFD=", tfd);
    log_hex("[ahci]   SERR=", serr);
    log_hex("[ahci]   CI=", ci);
    log_hex("[ahci]   SACT=", sact);
}

static int ahci_stop_port(u32 port) {
    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    u32 cmd = g_abar[(base + PxCMD) / 4];
    cmd &= ~AHCI_CMD_ST;
    g_abar[(base + PxCMD) / 4] = cmd;
    for (u32 i = 0; i < 100000; i++) {
        cmd = g_abar[(base + PxCMD) / 4];
        if (!(cmd & AHCI_CMD_CR)) break;
    }
    cmd &= ~AHCI_CMD_FRE;
    g_abar[(base + PxCMD) / 4] = cmd;
    for (u32 i = 0; i < 100000; i++) {
        cmd = g_abar[(base + PxCMD) / 4];
        if (!(cmd & AHCI_CMD_FR)) return 0;
    }
    return -1;
}

static int ahci_start_port(u32 port) {
    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    u32 cmd = g_abar[(base + PxCMD) / 4];
    cmd |= AHCI_CMD_FRE;
    g_abar[(base + PxCMD) / 4] = cmd;
    cmd |= AHCI_CMD_ST;
    g_abar[(base + PxCMD) / 4] = cmd;
    return 0;
}

static void ahci_fill_identify_fis(u8 *fis, u8 command) {
    for (u32 i = 0; i < 64; i++) fis[i] = 0;
    fis[0] = 0x27;       /* Register H2D FIS */
    fis[1] = 1u << 7;    /* command */
    fis[2] = command;
    fis[7] = 1u << 6;    /* LBA mode */
}

static void ahci_fill_read_fis(u8 *fis, u64 lba, u32 count) {
    ahci_zero(fis, 64);
    fis[0] = 0x27;
    fis[1] = 1u << 7;
    fis[2] = 0x25;       /* READ DMA EXT */
    fis[4] = (u8)(lba & 0xff);
    fis[5] = (u8)((lba >> 8) & 0xff);
    fis[6] = (u8)((lba >> 16) & 0xff);
    fis[7] = (1u << 6) | (u8)((lba >> 24) & 0x0f);
    fis[8] = (u8)((lba >> 24) & 0xff);
    fis[9] = (u8)((lba >> 32) & 0xff);
    fis[10] = (u8)((lba >> 40) & 0xff);
    fis[12] = (u8)(count & 0xff);
    fis[13] = (u8)((count >> 8) & 0xff);
}

static int ahci_read_blocks_internal(u32 port, ahci_cmd_header *hdr, struct dkm_dma_buffer *table, struct dkm_dma_buffer *data, u64 lba, u32 count, void *buffer) {
    if (!buffer || count == 0) return -1;
    if (count > 8) return -2;
    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    ahci_cmd_table *tbl = (ahci_cmd_table *)table->virt;
    u32 bytes = count * 512;
    ahci_zero(tbl, sizeof(ahci_cmd_table));
    ahci_zero(data->virt, bytes);

    hdr[0].flags = 5;
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;
    hdr[0].ctba = (u32)(table->phys & 0xffffffffu);
    hdr[0].ctbau = (u32)(table->phys >> 32);

    tbl->prdt[0].dba = (u32)(data->phys & 0xffffffffu);
    tbl->prdt[0].dbau = (u32)(data->phys >> 32);
    tbl->prdt[0].dbc_i = (bytes - 1) | (1u << 31);
    ahci_fill_read_fis(tbl->cfis, lba, count);

    g_abar[(base + PxIS) / 4] = 0xffffffffu;
    g_abar[HBA_IS / 4] = (1u << port);
    g_abar[(base + PxCI) / 4] = 1;
    for (u32 i = 0; i < 5000000; i++) {
        if ((g_abar[(base + PxCI) / 4] & 1u) == 0) {
            u8 *dst = (u8 *)buffer;
            u8 *src = (u8 *)data->virt;
            for (u32 j = 0; j < bytes; j++) dst[j] = src[j];
            return 0;
        }
    }
    g_log->warn("[ahci] READ timeout");
    log_hex("[ahci] READ lba=", lba);
    log_hex("[ahci] READ count=", count);
    log_hex("[ahci] READ CI=", g_abar[(base + PxCI) / 4]);
    log_hex("[ahci] READ IS=", g_abar[(base + PxIS) / 4]);
    log_hex("[ahci] READ TFD=", g_abar[(base + PxTFD) / 4]);
    return -3;
}

static int ahci_block_read(void *ctx, u64 lba, u32 count, void *buffer) {
    (void)ctx;
    if (g_ready_port == 0xffffffffu || !g_cmd_header) return -1;
    /* ensure port is in ST+FRE state */
    u32 base = HBA_PORT_BASE + g_ready_port * HBA_PORT_SIZE;
    u32 cmd = g_abar[(base + PxCMD) / 4];
    if (!(cmd & AHCI_CMD_ST) || !(cmd & AHCI_CMD_FRE)) {
        g_log->warn("[ahci] port not ready; restarting");
        ahci_stop_port(g_ready_port);
        ahci_start_port(g_ready_port);
    }
    while (count > 0) {
        u32 chunk = count > 8 ? 8 : count;
        int status = ahci_read_blocks_internal(g_ready_port, g_cmd_header, &g_cmd_table, &g_data, lba, chunk, buffer);
        if (status != 0) return status;
        lba += chunk;
        count -= chunk;
        buffer = (void *)((u8 *)buffer + (chunk * 512));
    }
    return 0;
}

static int ahci_read_lba0(u32 port, ahci_cmd_header *hdr, struct dkm_dma_buffer *table, struct dkm_dma_buffer *data) {
    int status = ahci_read_blocks_internal(port, hdr, table, data, 0, 1, data->virt);
    if (status == 0) {
        log_hex("[ahci] READ LBA0 port=", port);
        log_hex("[ahci] READ LBA0 bytes=", hdr[0].prdbc);
        log_hex("[ahci] READ LBA0 word0=", ((u16 *)data->virt)[0]);
        log_hex("[ahci] READ LBA0 word255=", ((u16 *)data->virt)[255]);
        return 1;
    }
    return status;
}

static int ahci_identify_port(u32 port, const struct dkm_dma_api *dma) {
    if (!dma || !dma->alloc_pages) return -1;

    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    u32 ssts = g_abar[(base + PxSSTS) / 4];
    u32 sig = g_abar[(base + PxSIG) / 4];
    u32 det = ssts & 0xf;
    u32 ipm = (ssts >> 8) & 0xf;
    if (det != 3 || ipm != 1) return 0;
    if (sig != 0x00000101u) {
        g_log->info("[ahci] IDENTIFY skipped non-SATA port");
        log_hex("[ahci] IDENTIFY skip port=", port);
        log_hex("[ahci] IDENTIFY skip sig=", sig);
        return 0;
    }

    struct dkm_dma_buffer clb;
    struct dkm_dma_buffer fis;
    struct dkm_dma_buffer table;
    struct dkm_dma_buffer data;
    if (dma->alloc_pages(1, 1024, 0x100000000ULL, &clb) != 0) return -2;
    if (dma->alloc_pages(1, 256, 0x100000000ULL, &fis) != 0) return -3;
    if (dma->alloc_pages(1, 128, 0x100000000ULL, &table) != 0) return -4;
    if (dma->alloc_pages(1, 512, 0x100000000ULL, &data) != 0) return -5;

    if (ahci_stop_port(port) != 0) {
        g_log->warn("[ahci] port stop timeout");
        return -6;
    }

    g_abar[(base + PxCLB) / 4] = (u32)(clb.phys & 0xffffffffu);
    g_abar[(base + PxCLB + 4) / 4] = (u32)(clb.phys >> 32);
    g_abar[(base + PxFB) / 4] = (u32)(fis.phys & 0xffffffffu);
    g_abar[(base + PxFB + 4) / 4] = (u32)(fis.phys >> 32);

    ahci_cmd_header *hdr = (ahci_cmd_header *)clb.virt;
    ahci_cmd_table *tbl = (ahci_cmd_table *)table.virt;
    hdr[0].flags = 5; /* FIS length in DWORDS */
    hdr[0].prdtl = 1;
    hdr[0].ctba = (u32)(table.phys & 0xffffffffu);
    hdr[0].ctbau = (u32)(table.phys >> 32);
    tbl->prdt[0].dba = (u32)(data.phys & 0xffffffffu);
    tbl->prdt[0].dbau = (u32)(data.phys >> 32);
    tbl->prdt[0].dbc_i = (512 - 1) | (1u << 31);

    u8 cmd = ATA_CMD_IDENTIFY;
    ahci_fill_identify_fis(tbl->cfis, cmd);

    g_abar[(base + PxIS) / 4] = 0xffffffffu;
    g_abar[HBA_IS / 4] = (1u << port);
    ahci_start_port(port);

    g_abar[(base + PxCI) / 4] = 1;
    for (u32 i = 0; i < 5000000; i++) {
        if ((g_abar[(base + PxCI) / 4] & 1u) == 0) {
            log_hex("[ahci] IDENTIFY port=", port);
            log_hex("[ahci] IDENTIFY status=", g_abar[(base + PxTFD) / 4]);
            log_hex("[ahci] IDENTIFY bytes=", hdr[0].prdbc);
            u16 *id = (u16 *)data.virt;
            log_hex("[ahci] IDENTIFY word0=", id[0]);
            log_hex("[ahci] IDENTIFY word49=", id[49]);
            log_hex("[ahci] IDENTIFY word83=", id[83]);
            ahci_read_lba0(port, hdr, &table, &data);
            g_ready_port = port;
            g_cmd_header = hdr;
            g_cmd_table = table;
            g_data = data;
            return 1;
        }
    }

    g_log->warn("[ahci] IDENTIFY timeout");
    log_hex("[ahci] IDENTIFY CI=", g_abar[(base + PxCI) / 4]);
    log_hex("[ahci] IDENTIFY IS=", g_abar[(base + PxIS) / 4]);
    log_hex("[ahci] IDENTIFY TFD=", g_abar[(base + PxTFD) / 4]);
    return -7;
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    g_log->info("[ahci] init begin");

    u8 bus = 0;
    u8 dev = 0;
    u8 func = 0;
    if (ahci_find(&bus, &dev, &func) != 0) {
        g_log->warn("[ahci] AHCI controller not found");
        g_log->info("[ahci] driver ready");
        return 0;
    }

    g_log->info("[ahci] AHCI controller found");
    log_hex("[ahci] bus=", bus);
    log_hex("[ahci] dev=", dev);
    log_hex("[ahci] func=", func);

    u32 vd = pci_read(bus, dev, func, PCI_VENDOR_ID);
    log_hex("[ahci] vendor=", vd & 0xffff);
    log_hex("[ahci] device=", vd >> 16);

    u32 command = pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_MEM | PCI_CMD_BUSM;
    pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[ahci] PCI command=", command);

    u64 abar_phys = (u64)(pci_read(bus, dev, func, PCI_BAR5) & 0xFFFFFFF0u);
    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[ahci] ABAR phys=", abar_phys);
    log_hex("[ahci] PCI IRQ line=", irq_line);

    if (!api->hhdm_offset || !abar_phys) {
        g_log->warn("[ahci] missing HHDM or ABAR; skip MMIO enumeration");
        g_log->info("[ahci] driver ready");
        return 0;
    }

    g_abar = (volatile u32 *)(uintptr_t)(api->hhdm_offset + abar_phys);
    log_hex("[ahci] HHDM offset=", api->hhdm_offset);

    u32 cap = g_abar[HBA_CAP / 4];
    u32 ghc = g_abar[HBA_GHC / 4];
    u32 is  = g_abar[HBA_IS / 4];
    u32 pi  = g_abar[HBA_PI / 4];
    u32 vs  = g_abar[HBA_VS / 4];
    u32 cap2 = g_abar[HBA_CAP2 / 4];
    u32 bohc = g_abar[HBA_BOHC / 4];

    log_hex("[ahci] CAP=", cap);
    log_hex("[ahci] GHC=", ghc);
    log_hex("[ahci] IS=", is);
    log_hex("[ahci] PI=", pi);
    log_hex("[ahci] VS=", vs);
    log_hex("[ahci] CAP2=", cap2);
    log_hex("[ahci] BOHC=", bohc);
    log_hex("[ahci] ports count=", (cap & 0x1f) + 1);
    log_hex("[ahci] cmd slots=", ((cap >> 8) & 0x1f) + 1);

    u32 implemented = 0;
    for (u32 port = 0; port < 32; port++) {
        if (pi & (1u << port)) {
            implemented++;
            ahci_log_port(port);
        }
    }
    log_hex("[ahci] implemented ports=", implemented);

    u32 identified = 0;
    if (api->dma) {
        for (u32 port = 0; port < 32; port++) {
            if (pi & (1u << port)) {
                int ident = ahci_identify_port(port, api->dma);
                if (ident > 0) {
                    identified++;
                    break;
                }
            }
        }
    } else {
        g_log->warn("[ahci] DMA API unavailable; skip IDENTIFY");
    }
    log_hex("[ahci] identified ports=", identified);
    if (identified && api->block && api->block->register_device) {
        struct dkm_block_device_desc desc;
        desc.name = "ahci0";
        desc.sector_size = 512;
        desc.sector_count = 0;
        desc.ctx = 0;
        desc.read = ahci_block_read;
        int index = api->block->register_device(&desc);
        log_hex("[ahci] block provider index=", (u64)(i64)index);
    } else if (identified) {
        g_log->warn("[ahci] block API unavailable; provider not registered");
    }

    g_log->info("[ahci] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
