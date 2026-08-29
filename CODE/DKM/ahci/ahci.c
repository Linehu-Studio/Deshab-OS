/* DKM AHCI Driver — PCI AHCI discovery and ABAR read-only enumeration
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 * This first version does not issue DMA commands; it only discovers the HBA
 * and ports so later block I/O can be added safely.
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(ahci_port_found);
DKM_STAT_DECL(ahci_identify_ok);
DKM_STAT_DECL(ahci_block_reads);
DKM_STAT_DECL(ahci_block_writes);
DKM_TS_DECL(ts_abar);

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
#define AHCI_CMD_W     (1u << 6)
#define AHCI_CMD_FR    (1u << 14)
#define AHCI_CMD_CR    (1u << 15)
#define AHCI_CMD_ICC_ACTIVE (1u << 28)

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35

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
/* IDENTIFY 读出的用户可寻址扇区数；LBA48 优先，回退 LBA28 */
static u64 g_sector_count;

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    return dkm_pci_read(bus, dev, func, reg);
}

static void pci_write(u8 bus, u8 dev, u8 func, u8 reg, u32 value) {
    dkm_pci_write(bus, dev, func, reg, value);
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
    /* PCI 规范允许 256 条 bus (0-255)，实机 AHCI 可能位于 bus > 7 */
    for (u16 bus = 0; bus < 256; bus++) {
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
    /* 实机: 500ms TSC 超时等待 CR 清除 */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 500;
    while (dkm_rdtsc() < deadline) {
        cmd = g_abar[(base + PxCMD) / 4];
        if (!(cmd & AHCI_CMD_CR)) break;
        __asm__ volatile("pause");
    }
    cmd &= ~AHCI_CMD_FRE;
    g_abar[(base + PxCMD) / 4] = cmd;
    /* 实机: 500ms TSC 超时等待 FR 清除 */
    deadline = dkm_rdtsc() + dkm_tsc_per_ms * 500;
    while (dkm_rdtsc() < deadline) {
        cmd = g_abar[(base + PxCMD) / 4];
        if (!(cmd & AHCI_CMD_FR)) return 0;
        __asm__ volatile("pause");
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
    fis[2] = ATA_CMD_READ_DMA_EXT;
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

static void ahci_fill_write_fis(u8 *fis, u64 lba, u32 count) {
    ahci_zero(fis, 64);
    fis[0] = 0x27;
    fis[1] = 1u << 7;
    fis[2] = ATA_CMD_WRITE_DMA_EXT;
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
    /* TSC deadline 超时: 5 秒，实机机械 HDD 首次寻道需数百 ms */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 5000;
    while (dkm_rdtsc() < deadline) {
        if ((g_abar[(base + PxCI) / 4] & 1u) == 0) {
            u8 *dst = (u8 *)buffer;
            u8 *src = (u8 *)data->virt;
            for (u32 j = 0; j < bytes; j++) dst[j] = src[j];
            return 0;
        }
        __asm__ volatile("pause");
    }
    g_log->warn("[ahci] READ timeout");
    log_hex("[ahci] READ lba=", lba);
    log_hex("[ahci] READ count=", count);
    log_hex("[ahci] READ CI=", g_abar[(base + PxCI) / 4]);
    log_hex("[ahci] READ IS=", g_abar[(base + PxIS) / 4]);
    log_hex("[ahci] READ TFD=", g_abar[(base + PxTFD) / 4]);
    return -3;
}

static int ahci_write_blocks_internal(u32 port, ahci_cmd_header *hdr, struct dkm_dma_buffer *table, struct dkm_dma_buffer *data, u64 lba, u32 count, const void *buffer) {
    if (!buffer || count == 0) return -1;
    if (count > 8) return -2;
    u32 base = HBA_PORT_BASE + port * HBA_PORT_SIZE;
    ahci_cmd_table *tbl = (ahci_cmd_table *)table->virt;
    u32 bytes = count * 512;
    ahci_zero(tbl, sizeof(ahci_cmd_table));

    /* copy data from user buffer into DMA buffer */
    {
        u8 *dst = (u8 *)data->virt;
        const u8 *src = (const u8 *)buffer;
        for (u32 j = 0; j < bytes; j++) dst[j] = src[j];
    }

    /* BUG-P0-2: 写入前必须 flush CPU cache，否则 DMA 控制器从物理内存
     * 读到陈旧数据（CPU write-back cache 尚未写回）。实机必现。 */
    __asm__ volatile("wbinvd" ::: "memory");

    hdr[0].flags = 5 | (1u << 6);  /* CFL=5, W=1 (write) */
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;
    hdr[0].ctba = (u32)(table->phys & 0xffffffffu);
    hdr[0].ctbau = (u32)(table->phys >> 32);

    tbl->prdt[0].dba = (u32)(data->phys & 0xffffffffu);
    tbl->prdt[0].dbau = (u32)(data->phys >> 32);
    tbl->prdt[0].dbc_i = (bytes - 1) | (1u << 31);
    ahci_fill_write_fis(tbl->cfis, lba, count);

    /* P0 修复: 移除 PxCMD.W 写入。AHCI 规范中 PxCMD.W 是只读位，
     * 由硬件根据命令头 flags.W 自动设置。软件写入此位违反规范，
     * 可能在某些实机 AHCI 控制器上导致未定义行为。 */
    /* (原代码: cmd |= AHCI_CMD_W; g_abar[...] = cmd; — 已删除) */

    g_abar[(base + PxIS) / 4] = 0xffffffffu;
    g_abar[HBA_IS / 4] = (1u << port);
    g_abar[(base + PxCI) / 4] = 1;
    /* P0 修复: 写入超时改为 TSC deadline，与读取路径一致。
     * 固定循环在实机 HDD 上会因 CPU 频率差异导致提前超时或死循环。 */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 5000;
    while (dkm_rdtsc() < deadline) {
        if ((g_abar[(base + PxCI) / 4] & 1u) == 0) {
            /* BUG-P1-1 fix: CI 清除后检查 TFD 错误位。
             * TFD 低字节是 STS，正常值 0x50 (DRDY|DRQ) 非零但不是错误。
             * 只有 STS bit0 (ERR) 或 ERR 寄存器 (bits15:8) 非零才是真正错误。
             * BUG-20260730-P14: 原 `tfd & 0xFF` 误判 STS=0x50 为错误，导致写入
             * 实际成功却被报告为失败，FAT32 写路径提前返回错误码。 */
            u32 tfd = g_abar[(base + PxTFD) / 4];
            u32 is  = g_abar[(base + PxIS) / 4];
            u8 sts = (u8)(tfd & 0xFF);
            u8 err_reg = (u8)((tfd >> 8) & 0xFF);
            if ((sts & 0x01) || err_reg) {
                g_log->warn("[ahci] write error: TFD ERR");
                log_hex("[ahci]   TFD=", tfd);
                log_hex("[ahci]   PxIS=", is);
                return -4;
            }
            return 0;
        }
        __asm__ volatile("pause");
    }

    g_log->warn("[ahci] WRITE timeout");
    log_hex("[ahci] WRITE lba=", lba);
    log_hex("[ahci] WRITE count=", count);
    log_hex("[ahci] WRITE CI=", g_abar[(base + PxCI) / 4]);
    log_hex("[ahci] WRITE IS=", g_abar[(base + PxIS) / 4]);
    log_hex("[ahci] WRITE TFD=", g_abar[(base + PxTFD) / 4]);
    return -3;
}

static int ahci_block_read(void *ctx, u64 lba, u32 count, void *buffer) {
    (void)ctx;
    if (g_ready_port == 0xffffffffu || !g_cmd_header) return -1;
    DKM_STAT_INC(ahci_block_reads);
    log_hex("[ahci] block_read lba=", lba);
    log_hex("[ahci] block_read count=", count);
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

static int ahci_block_write(void *ctx, u64 lba, u32 count, const void *buffer) {
    (void)ctx;
    if (g_ready_port == 0xffffffffu || !g_cmd_header) return -1;
    DKM_STAT_INC(ahci_block_writes);
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
        int status = ahci_write_blocks_internal(g_ready_port, g_cmd_header, &g_cmd_table, &g_data, lba, chunk, buffer);
        if (status != 0) return status;
        lba += chunk;
        count -= chunk;
        buffer = (const void *)((const u8 *)buffer + (chunk * 512));
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
    /* BUG-P0-1: sizeof(ahci_cmd_table)=144, 128B 缓冲区越界 16 字节。
     * 改为 256B 满足对齐和大小要求，防止 ahci_zero 越界写。 */
    if (dma->alloc_pages(1, 256, 0x100000000ULL, &table) != 0) return -4;
    /* BUG-20260730-022: DMA data buffer must hold at least 8 sectors (4096B)
	 * for ahci_block_write chunked DMA.  512B caused 3584B out-of-bounds
	 * overwrite of adjacent kernel data on real hardware. */
	if (dma->alloc_pages(1, 8192, 0x100000000ULL, &data) != 0) return -5;

    /* P0 验证: DMA 缓冲区物理地址必须在 4GB 以下。
     * AHCI 控制器 8086:2922 只支持 32 位 DMA，
     * PRDT 地址超过 4GB 会导致 DMA 写入截断到错误物理地址。
     * dma->alloc_pages 的上限参数 0x100000000ULL 已保证此约束，
     * 但添加显式校验以防御 alloc_pages 实现变更。 */
    if (clb.phys >= 0x100000000ULL || fis.phys >= 0x100000000ULL ||
        table.phys >= 0x100000000ULL || data.phys >= 0x100000000ULL) {
        g_log->warn("[ahci] DMA buffer above 4GB — 32-bit DMA will fail on this controller!");
        log_hex("[ahci] clb.phys=", clb.phys);
        log_hex("[ahci] fis.phys=", fis.phys);
        log_hex("[ahci] table.phys=", table.phys);
        log_hex("[ahci] data.phys=", data.phys);
    }

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
    /* BUG-P0-3: 固定 500 万次循环在 3GHz CPU 上仅 ~1-2ms，
     * 不够 HDD 寻道。改为 TSC deadline 5s 超时，与 read/write 路径一致。 */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 5000;
    while (dkm_rdtsc() < deadline) {
        if ((g_abar[(base + PxCI) / 4] & 1u) == 0) {
            log_hex("[ahci] IDENTIFY port=", port);
            log_hex("[ahci] IDENTIFY status=", g_abar[(base + PxTFD) / 4]);
            log_hex("[ahci] IDENTIFY bytes=", hdr[0].prdbc);
            u16 *id = (u16 *)data.virt;
            log_hex("[ahci] IDENTIFY word0=", id[0]);
            log_hex("[ahci] IDENTIFY word49=", id[49]);
            log_hex("[ahci] IDENTIFY word83=", id[83]);
            /* 容量必须在 ahci_read_lba0 复用 data 缓冲区前提取，
             * 否则 IDENTIFY 数据被 LBA0 内容覆盖后容量丢失。 */
            if ((id[83] & 0x0400u) && (id[103] | id[102] | id[101] | id[100])) {
                /* LBA48: word100-103 小端 u16 序列拼接 */
                g_sector_count = (u64)id[100] | ((u64)id[101] << 16)
                               | ((u64)id[102] << 32) | ((u64)id[103] << 48);
            } else {
                /* LBA28: word60-61 */
                g_sector_count = (u64)id[60] | ((u64)id[61] << 16);
            }
            log_hex("[ahci] IDENTIFY sector_count=", g_sector_count);
            ahci_read_lba0(port, hdr, &table, &data);
            g_ready_port = port;
            g_cmd_header = hdr;
            g_cmd_table = table;
            g_data = data;
            return 1;
        }
        __asm__ volatile("pause");
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
    /* 实机要求: 先校准 TSC, 再使用基于 CPU 频率的延迟 */
    dkm_tsc_calibrate();

    if (!api || !api->log) return -1;
    g_log = api->log;
    dkm_instr_init(api);

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

    /* 读取 ABAR: BAR5 可能是 64-bit MMIO BAR，高位在 BAR4 */
    u32 bar5_lo = pci_read(bus, dev, func, PCI_BAR5);
    u64 abar_phys;
    if ((bar5_lo & 0x7) == 0x4) {
        /* 64-bit MMIO BAR: 高 32 位在 BAR4 */
        u32 bar5_hi = pci_read(bus, dev, func, PCI_BAR4);
        abar_phys = ((u64)bar5_hi << 32) | (bar5_lo & 0xFFFFFFF0u);
        g_log->info("[ahci] ABAR is 64-bit MMIO BAR");
    } else {
        abar_phys = bar5_lo & 0xFFFFFFF0u;
    }
    u32 irq_line = pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[ahci] ABAR phys=", abar_phys);
    log_hex("[ahci] PCI IRQ line=", irq_line);

    if (!api->hhdm_offset || !abar_phys) {
        g_log->warn("[ahci] missing HHDM or ABAR; skip MMIO enumeration");
        g_log->info("[ahci] driver ready");
        return 0;
    }

    g_abar = (volatile u32 *)(uintptr_t)(api->hhdm_offset + abar_phys);
    DKM_TS_BEGIN(ts_abar);
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

    /* AHCI 1.3+ BOHC handoff: 如果 BIOS 仍拥有控制器, 请求所有权 */
    if (bohc & 0x01) {
        g_log->info("[ahci] BIOS owns HBA, requesting handoff");
        g_abar[HBA_BOHC / 4] = bohc | 0x02;  /* set OOS */
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 2000;
        while (dkm_rdtsc() < deadline) {
            if (!(g_abar[HBA_BOHC / 4] & 0x01)) break;
            __asm__ volatile("pause");
        }
        if (g_abar[HBA_BOHC / 4] & 0x01) {
            g_log->warn("[ahci] BIOS handoff timeout, proceeding anyway");
        } else {
            g_log->info("[ahci] BIOS handoff complete");
        }
    }

    log_hex("[ahci] ports count=", (cap & 0x1f) + 1);
    log_hex("[ahci] cmd slots=", ((cap >> 8) & 0x1f) + 1);
    DKM_TS_END(ts_abar, g_log->info, "ahci_abar_mmio");

    u32 implemented = 0;
    for (u32 port = 0; port < 32; port++) {
        if (pi & (1u << port)) {
            implemented++;
            DKM_STAT_INC(ahci_port_found);
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
                    DKM_STAT_INC(ahci_identify_ok);
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
        desc.sector_count = g_sector_count;
        desc.ctx = 0;
        desc.read = ahci_block_read;
        desc.write = ahci_block_write;
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
