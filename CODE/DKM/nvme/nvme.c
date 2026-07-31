/* DKM NVMe Driver — 高位 BAR MMIO 映射 + admin/IO queue + block provider
 *
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 *
 * 实现要点:
 *   1. 64-bit BAR 常位于 4G 以上（QEMU/OVMF 把 NVMe BAR0 放到 ~48GiB），
 *      HHDM 直映射不保证覆盖。访问前先用 kernel_api.mmio->is_mapped 做
 *      四级页表 walk 核实；未映射则 map_mmio 原地补建 4KiB UC 页。
 *   2. 同步轮询模型: 单 outstanding 命令 + CQE phase bit 轮询 + TSC 超时,
 *      不依赖 IRQ/MSI（当前 PIC/APIC 后端未完工）。
 *   3. 数据面: 32 页 (128KiB) 物理连续 DMA bounce buffer + 单页 PRP list。
 *      PRP 布局按传输页数分档: 1 页单 PRP1; 2 页 PRP2 直连第二页;
 *      >=3 页 PRP2 指向 PRP list 页 (512 项 u64, 上限 513 页 = 2052KiB)。
 *      chunk 上限 = min(buffer 页数, 2^MDTS 页, NLB 16-bit)。
 *      读写一律 bounce: 调用方缓冲是任意虚拟地址 (DSK BSS / vmm dbuf),
 *      物理底账未知, 统一经 DMA 缓冲中转, 一次命令读满 chunk 再 memcpy。
 *      WRITE 路径 memcpy 后 wbinvd (沿用 ahci BUG-P0-2 防陈旧 cache 先例),
 *      每次写调用末尾随一个 FLUSH。admin/IO queue 各 64 项, 均每队列一页。
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(nvm0_pci_found);

static const char *const g_depends[] = { "pci", "irq" };
static const char *const g_provides[] = { "block" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "nvme",
    .version        = "0.3.0",
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

/* ---- NVMe 控制器寄存器 ---- */
#define NVME_CAP      0x00    /* u64 capabilities */
#define NVME_VS       0x08    /* u32 version */
#define NVME_INTMS    0x0C    /* interrupt mask set */
#define NVME_INTMC    0x10    /* interrupt mask clear */
#define NVME_CC       0x14    /* controller configuration */
#define NVME_CSTS     0x1C    /* controller status */
#define NVME_AQA      0x24    /* admin queue attributes */
#define NVME_ASQ      0x28    /* u64 admin SQ base */
#define NVME_ACQ      0x30    /* u64 admin CQ base */
#define NVME_DB_BASE  0x1000  /* doorbell 起始偏移 */

#define NVME_CLASS_STORAGE 0x01
#define NVME_SUBCLASS_NVM  0x08
#define NVME_PROGIF_NVME   0x02

/* admin 命令 opcode */
#define NVME_ADM_DELETE_IOSQ  0x00
#define NVME_ADM_CREATE_IOSQ  0x01
#define NVME_ADM_DELETE_IOCQ  0x04
#define NVME_ADM_CREATE_IOCQ  0x05
#define NVME_ADM_IDENTIFY     0x06
/* IO 命令 opcode（NVM 命令集: 00h=Flush, 01h=Write, 02h=Read） */
#define NVME_IO_FLUSH         0x00
#define NVME_IO_READ          0x02
#define NVME_IO_WRITE         0x01

#define ADMIN_QSIZE  64       /* entry 数（SQ 64*64B=4K 一页, CQ 64*16B=1K） */
#define IO_QSIZE     64
#define NVME_IDENT_BYTES 4096           /* identify 数据页 */
#define NVME_DATA_PAGES  32u            /* IO bounce: 128KiB = 512B×256 扇区,
                                         * 正好覆盖 DSK FAT32 的 256 扇区批量读 */
#define NVME_PRP_LIST_ENTRIES 512u      /* 单 list 页 4KiB / 8B = 512 项 */

static const struct dkm_log_api *g_log;
static volatile u8 *g_regs;             /* MMIO 基址（虚拟） */
static u32 g_db_stride;                 /* doorbell 步长 = 4 << CAP.DSTRD */
static u64 g_timeout_ms;                /* CAP.TO * 500ms */

static struct dkm_dma_buffer g_asq;     /* admin SQ */
static struct dkm_dma_buffer g_acq;     /* admin CQ */
static struct dkm_dma_buffer g_ident;   /* identify 数据页 */
static struct dkm_dma_buffer g_data;    /* IO 读写 bounce (NVME_DATA_PAGES 页连续) */
static struct dkm_dma_buffer g_prp;     /* PRP list 页 */
static struct dkm_dma_buffer g_iosq;    /* IO SQ (qid=1) */
static struct dkm_dma_buffer g_iocq;    /* IO CQ (qid=1) */

static u32 g_asq_tail;
static u32 g_acq_head;
static u32 g_acq_phase;
static u32 g_iosq_tail;
static u32 g_iocq_head;
static u32 g_iocq_phase;
static u16 g_cid;

static u64 g_sector_size;
static u64 g_sector_count;
static u32 g_mdts;                      /* Identify Controller byte 77 */
static u32 g_chunk_max_sectors;         /* 单命令扇区上限 (buffer/MDTS/NLB 取小) */
static int g_io_ready;
static int g_test_disk;                 /* LBA0 带 DESHABNVME0 签名的测试盘 */

/* ---- 基础工具 ---- */

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

/* 打印有界 ASCII 串（identify 数据中的 SN/MN 等定长字段） */
static void log_str(const char *prefix, const u8 *src, u32 len) {
    char buf[48];
    if (len > 40) len = 40;
    for (u32 i = 0; i < len; i++) {
        char c = (char)src[i];
        buf[i] = (c >= 32 && c < 127) ? c : ' ';
    }
    buf[len] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static void nvme_zero(void *ptr, u32 len) {
    u8 *p = (u8 *)ptr;
    for (u32 i = 0; i < len; i++) p[i] = 0;
}

static __inline__ void nvme_mb(void) {
    /* x86 TSO 保证 store-store 顺序, 这里只需阻断编译器重排 */
    __asm__ volatile("" ::: "memory");
}

static u32 mmio_read32(u32 off) {
    return *(volatile u32 *)(void *)(g_regs + off);
}

static void mmio_write32(u32 off, u32 value) {
    *(volatile u32 *)(void *)(g_regs + off) = value;
}

static u64 mmio_read64(u32 off) {
    u32 lo = mmio_read32(off);
    u32 hi = mmio_read32(off + 4);
    return ((u64)hi << 32) | lo;
}

static void mmio_write64(u32 off, u64 value) {
    mmio_write32(off, (u32)(value & 0xffffffffu));
    mmio_write32(off + 4, (u32)(value >> 32));
}

static u32 sq_tail_db(u16 qid) { return NVME_DB_BASE + (2u * qid) * g_db_stride; }
static u32 cq_head_db(u16 qid) { return NVME_DB_BASE + (2u * qid + 1u) * g_db_stride; }

/* ---- PCI ---- */

static int nvme_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
    for (u8 bus = 0; bus < 16; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = dkm_pci_read(bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;

            u8 header = (u8)(dkm_pci_read(bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;
            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = dkm_pci_read(bus, dev, func, PCI_VENDOR_ID);
                if ((vd2 & 0xffff) == 0xffff) continue;

                u32 class_reg = dkm_pci_read(bus, dev, func, 0x08);
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

/* ---- CSTS 等待 ---- */

static int nvme_wait_csts(u32 mask, u32 expect, u64 timeout_ms) {
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * timeout_ms;
    for (;;) {
        u32 csts = mmio_read32(NVME_CSTS);
        if (csts & (1u << 1)) {            /* CFS: controller fatal status */
            g_log->error("[nvme] CSTS.CFS set");
            log_hex("[nvme] CSTS=", csts);
            return -2;
        }
        if ((csts & mask) == expect) return 0;
        if (dkm_rdtsc() > deadline) {
            g_log->error("[nvme] CSTS wait timeout");
            log_hex("[nvme] CSTS=", csts);
            log_hex("[nvme] wait mask=", mask);
            return -1;
        }
        __asm__ volatile("pause");
    }
}

/* 前向声明: nvme_dump_regs 在 nvme_submit 中使用 */
static void nvme_dump_regs(const char *ctx);

/* ---- 命令提交（同步轮询, 单 outstanding） ----
 * dw[16] 为完整 SQE（dw0 低 16 位不含 CID, 由本函数填入）。
 * 返回 0 成功；>0 = NVMe status code；<0 超时/内部错误。
 */
static int nvme_submit(volatile u32 *sq, u32 *sq_tail, u16 sq_qid,
                       volatile u32 *cq, u32 *cq_head, u32 *cq_phase,
                       u16 cq_qid, u32 qsize, u32 *dw, u32 *out_cqe_dw0) {
    dw[0] = (dw[0] & 0xffffu) | ((u32)g_cid << 16);
    u32 cid = g_cid;
    g_cid++;

    /* 逐 dword 写入 SQE（64B），避免 freestanding 下编译器生成 memcpy 调用 */
    u32 pos = *sq_tail;
    for (u32 i = 0; i < 16; i++) sq[pos * 16 + i] = dw[i];

    *sq_tail = (pos + 1) % qsize;
    nvme_mb();                                  /* SQ 内容先于 doorbell 可见 */
    mmio_write32(sq_tail_db(sq_qid), *sq_tail);

    /* 真机超时: 使用 CAP.TO * 500ms * 10 (留 10 倍余量), 下限 5s。
     * 真机 NVMe 控制器在重负载下可能需要更长的完成时间。 */
    u64 submit_timeout_ms = g_timeout_ms * 10;
    if (submit_timeout_ms < 5000) submit_timeout_ms = 5000;
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * submit_timeout_ms;
    for (;;) {
        u32 dw3 = cq[*cq_head * 4 + 3];
        if (((dw3 >> 16) & 1u) == *cq_phase) {
            u32 sc  = (dw3 >> 17) & 0xffu;
            u32 sct = (dw3 >> 25) & 0x7u;
            if (out_cqe_dw0) *out_cqe_dw0 = cq[*cq_head * 4 + 0];

            *cq_head = (*cq_head + 1) % qsize;
            if (*cq_head == 0) *cq_phase ^= 1u;
            nvme_mb();
            mmio_write32(cq_head_db(cq_qid), *cq_head);

            if (sc || sct) {
                g_log->error("[nvme] command failed");
                log_hex("[nvme] opcode=", dw[0] & 0xffu);
                log_hex("[nvme] cid=", cid);
                log_hex("[nvme] SC=", sc);
                log_hex("[nvme] SCT=", sct);
                /* SC 与 SCT 必须联合编码: SCT!=0 且 SC==0 时只返回 sc
                 * 会返回 0 被调用方误判为成功（CQID_INVALID 正是此类）。 */
                return (int)(sc | (sct << 8));
            }
            return 0;
        }
        if (dkm_rdtsc() > deadline) {
            g_log->error("[nvme] completion timeout");
            log_hex("[nvme] opcode=", dw[0] & 0xffu);
            nvme_dump_regs("completion timeout");
            return -100;
        }
        __asm__ volatile("pause");
    }
}

static int nvme_admin_submit(u32 *dw, u32 *out_cqe_dw0) {
    return nvme_submit((volatile u32 *)g_asq.virt, &g_asq_tail, 0,
                       (volatile u32 *)g_acq.virt, &g_acq_head, &g_acq_phase,
                       0, ADMIN_QSIZE, dw, out_cqe_dw0);
}

static int nvme_io_submit(u32 *dw, u32 *out_cqe_dw0) {
    return nvme_submit((volatile u32 *)g_iosq.virt, &g_iosq_tail, 1,
                       (volatile u32 *)g_iocq.virt, &g_iocq_head, &g_iocq_phase,
                       1, IO_QSIZE, dw, out_cqe_dw0);
}

/* ---- Identify ---- */

static int nvme_identify_controller(u32 *out_nn) {
    u32 dw[16];
    nvme_zero(dw, sizeof(dw));
    nvme_zero(g_ident.virt, NVME_IDENT_BYTES);

    dw[0] = NVME_ADM_IDENTIFY;
    dw[1] = 0;                          /* NSID=0 for controller */
    dw[6] = (u32)(g_ident.phys & 0xffffffffu);
    dw[7] = (u32)(g_ident.phys >> 32);
    dw[10] = 1;                         /* CNS=1: identify controller */

    int rc = nvme_admin_submit(dw, 0);
    if (rc != 0) return rc;

    u8 *id = (u8 *)g_ident.virt;
    log_str("[nvme] ctrl SN=", id + 4, 20);
    log_str("[nvme] ctrl MN=", id + 24, 40);
    log_str("[nvme] ctrl FR=", id + 64, 8);
    /* Identify Controller 布局锚点核对（NVMe 1.4, CNS=1 数据结构）:
     *   byte 77      MDTS  u8   最大传输 = 2^MDTS × CAP.MPSMIN 页, 0=无限制
     *   byte 512     SQES/CQES/MAXCMD —— QEMU 实测 0x00004466, 证明偏移基正确
     *   byte 516     NN    u32  Number of Namespaces
     * NN 核实结论 (QEMU 11.0 串口实证): NN=0x100 不是解析错位。NN 的 spec
     * 语义是"控制器支持的最大合法 NSID 数"而非已挂载数量; QEMU hw/nvme
     * 填的是 NVME_MAX_NAMESPACES=256 (0x100)。实际挂载的 NSID=1 由后续
     * Identify Namespace(CNS=0) 成功返回确认。解析偏移 516 与 spec 一致,
     * 无需修正。 */
    u32 anchor512 = (u32)id[512] | ((u32)id[513] << 8) |
                    ((u32)id[514] << 16) | ((u32)id[515] << 24);
    log_hex("[nvme] ID dword@512(SQES/CQES/MAXCMD)=", anchor512);
    u32 nn = (u32)id[516] | ((u32)id[517] << 8) | ((u32)id[518] << 16) | ((u32)id[519] << 24);
    log_hex("[nvme] NN(max valid NSID)=", nn);
    log_hex("[nvme] MDTS=", id[77]);
    g_mdts = id[77];
    *out_nn = nn;
    return 0;
}

static int nvme_identify_namespace(u32 nsid, u64 *out_nsze, u32 *out_lbads) {
    u32 dw[16];
    nvme_zero(dw, sizeof(dw));
    nvme_zero(g_ident.virt, NVME_IDENT_BYTES);

    dw[0] = NVME_ADM_IDENTIFY;
    dw[1] = nsid;
    dw[6] = (u32)(g_ident.phys & 0xffffffffu);
    dw[7] = (u32)(g_ident.phys >> 32);
    dw[10] = 0;                         /* CNS=0: identify namespace */

    int rc = nvme_admin_submit(dw, 0);
    if (rc != 0) return rc;

    u8 *id = (u8 *)g_ident.virt;
    u64 nsze = 0;
    for (u32 i = 0; i < 8; i++) nsze |= ((u64)id[i]) << (i * 8);
    u8 flbas = id[26] & 0x0f;
    u32 lbaf_off = 128 + (u32)flbas * 4;
    u32 lbaf = (u32)id[lbaf_off] | ((u32)id[lbaf_off + 1] << 8) |
               ((u32)id[lbaf_off + 2] << 16) | ((u32)id[lbaf_off + 3] << 24);
    u32 lbads = (lbaf >> 16) & 0xff;

    log_hex("[nvme] NSZE=", nsze);
    log_hex("[nvme] FLBAS=", flbas);
    log_hex("[nvme] LBADS=", lbads);

    *out_nsze = nsze;
    *out_lbads = lbads;
    return 0;
}

/* ---- IO queue 创建 ---- */

static int nvme_create_io_queues(void) {
    u32 dw[16];

    /* Create IO CQ (qid=1): PC=1, IEN=0 (轮询模式)
     * CDW10 = QSIZE[31:16](0's based) | QID[15:0] —— QID 必须在低半字 */
    nvme_zero(dw, sizeof(dw));
    dw[0] = NVME_ADM_CREATE_IOCQ;
    dw[6] = (u32)(g_iocq.phys & 0xffffffffu);
    dw[7] = (u32)(g_iocq.phys >> 32);
    dw[10] = ((u32)(IO_QSIZE - 1u) << 16) | 1u;
    dw[11] = 1;
    int rc = nvme_admin_submit(dw, 0);
    if (rc != 0) {
        g_log->error("[nvme] create IO CQ failed");
        return rc;
    }

    /* Create IO SQ (qid=1, cqid=1): PC=1
     * CDW10 同 Create IOCQ: QSIZE[31:16] | QID[15:0]; CDW11 = CQID[31:16] | PC */
    nvme_zero(dw, sizeof(dw));
    dw[0] = NVME_ADM_CREATE_IOSQ;
    dw[6] = (u32)(g_iosq.phys & 0xffffffffu);
    dw[7] = (u32)(g_iosq.phys >> 32);
    dw[10] = ((u32)(IO_QSIZE - 1u) << 16) | 1u;
    dw[11] = (1u << 16) | 1;
    rc = nvme_admin_submit(dw, 0);
    if (rc != 0) {
        g_log->error("[nvme] create IO SQ failed");
        return rc;
    }

    g_iosq_tail = 0;
    g_iocq_head = 0;
    g_iocq_phase = 1;
    return 0;
}

/* ---- IO Read/Write（PRP1 + PRP2 直连 / PRP list 分档） ----
 * buf_phys 必须 4KiB 对齐且物理连续（本驱动只传 g_data.phys）。
 * 传输页数由 nlb × sector_size 推出，决定 PRP 布局:
 *   1 页  : PRP2 = 0（单 PRP1，等价旧 bounce 路径）
 *   2 页  : PRP2 = 第二页物理地址（spec 规定的两页直连形式）
 *   >=3 页: PRP2 = g_prp list 页物理地址, list[i] = 第 i+2 页物理地址
 * 单 list 页 512 项, 上限 513 页 (2052KiB), 远大于 chunk 上限 32 页,
 * 无需 list 链页。PRP list 由 CPU 写、控制器 DMA 读: x86 TSO + 编译屏障
 * 保证 list 先于 doorbell 可见（与 SQE 提交路径同一约定）。
 */
static int nvme_io_rw(u8 opcode, u64 slba, u32 nlb, u64 buf_phys) {
    u32 bytes = nlb * (u32)g_sector_size;
    u32 pages = (bytes + 4095u) >> 12;
    if (pages == 0 || (pages - 1u) > NVME_PRP_LIST_ENTRIES) return -3;

    u32 dw[16];
    nvme_zero(dw, sizeof(dw));
    dw[0] = opcode;
    dw[1] = 1;                          /* nsid=1 */
    dw[6] = (u32)(buf_phys & 0xffffffffu);
    dw[7] = (u32)(buf_phys >> 32);
    if (pages == 2) {
        u64 p2 = buf_phys + 4096u;
        dw[8] = (u32)(p2 & 0xffffffffu);
        dw[9] = (u32)(p2 >> 32);
    } else if (pages >= 3) {
        volatile u64 *list = (volatile u64 *)g_prp.virt;
        for (u32 i = 0; i < pages - 1u; i++)
            list[i] = buf_phys + 4096ULL * (u64)(i + 1u);
        nvme_mb();                      /* list 内容先于 doorbell 可见 */
        dw[8] = (u32)(g_prp.phys & 0xffffffffu);
        dw[9] = (u32)(g_prp.phys >> 32);
    }
    dw[10] = (u32)(slba & 0xffffffffu);
    dw[11] = (u32)(slba >> 32);
    dw[12] = nlb - 1u;                  /* NLB 0-based */
    return nvme_io_submit(dw, 0);
}

/* NVM Flush (opcode 0x00): 无数据字段, 把 volatile write cache 刷到介质 */
static int nvme_io_flush(void) {
    u32 dw[16];
    nvme_zero(dw, sizeof(dw));
    dw[0] = NVME_IO_FLUSH;
    dw[1] = 1;                          /* nsid=1 */
    return nvme_io_submit(dw, 0);
}

/* ---- block provider ----
 * 调用方缓冲为任意虚拟地址（DSK 静态 BSS、vmm virtio_blk dbuf 等），
 * 物理底账未知且不一定连续/对齐，不能直接进 PRP。一律 bounce 经
 * g_data（物理连续 DMA 区）: 一次命令读/写满 chunk，再 memcpy 中转。
 */

static int nvme_block_read(void *ctx, u64 lba, u32 count, void *buffer) {
    (void)ctx;
    if (!g_io_ready || !buffer || count == 0) return -1;
    u8 *out = (u8 *)buffer;
    while (count > 0) {
        u32 n = count > g_chunk_max_sectors ? g_chunk_max_sectors : count;
        int rc = nvme_io_rw(NVME_IO_READ, lba, n, g_data.phys);
        if (rc != 0) return rc;
        u32 bytes = n * (u32)g_sector_size;
        u8 *src = (u8 *)g_data.virt;
        for (u32 i = 0; i < bytes; i++) out[i] = src[i];
        out += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

static int nvme_block_write(void *ctx, u64 lba, u32 count, const void *buffer) {
    (void)ctx;
    if (!g_io_ready || !buffer || count == 0) return -1;
    const u8 *in = (const u8 *)buffer;
    while (count > 0) {
        u32 n = count > g_chunk_max_sectors ? g_chunk_max_sectors : count;
        u32 bytes = n * (u32)g_sector_size;
        u8 *dst = (u8 *)g_data.virt;
        for (u32 i = 0; i < bytes; i++) dst[i] = in[i];
        /* 沿用 ahci BUG-P0-2 先例: 写前 flush CPU cache, 防 DMA 读到陈旧数据 */
        __asm__ volatile("wbinvd" ::: "memory");
        int rc = nvme_io_rw(NVME_IO_WRITE, lba, n, g_data.phys);
        if (rc != 0) return rc;
        in += bytes;
        lba += n;
        count -= n;
    }
    /* 每次写调用末尾 FLUSH 一次，保证 volatile write cache 落盘 */
    return nvme_io_flush();
}

/* ---- 数据面自测（仅签名测试盘运行写测试，防实机盘数据损坏） ---- */

/* 读回 LBA0 并核对 DESHABNVME0 签名（写测试后复查无串扰用） */
static int nvme_lba0_sig_ok(void) {
    static const char sig[] = "DESHABNVME0";
    int rc = nvme_io_rw(NVME_IO_READ, 0, 1, g_data.phys);
    if (rc != 0) return 0;
    u8 *d = (u8 *)g_data.virt;
    for (u32 i = 0; i < 11; i++) {
        if (d[i] != (u8)sig[i]) return 0;
    }
    return 1;
}

/* 多页单命令读: LBA0 起 32 扇区 = 16KiB = 4 页 → PRP list 路径。
 * 测试盘内容已知: LBA0 为 "DESHABNVME0" 重复填满 512B, LBA1..31 全零。 */
static void nvme_selftest_read_multipage(void) {
    static u8 rbuf[16384];
    static const char sig[] = "DESHABNVME0";
    int rc = nvme_block_read(0, 0, 32, rbuf);
    log_hex("[nvme] PRPLIST read 32sec rc=", (u64)(i64)rc);
    if (rc != 0) return;

    u32 bad = 0;
    for (u32 i = 0; i < 512; i++) {
        if (rbuf[i] != (u8)sig[i % 11]) {
            bad = 1;
            log_hex("[nvme]   sig mismatch idx=", i);
            break;
        }
    }
    if (!bad) {
        for (u32 i = 512; i < 16384; i++) {
            if (rbuf[i] != 0) {
                bad = 1;
                log_hex("[nvme]   nonzero idx=", i);
                break;
            }
        }
    }
    if (bad) {
        g_log->error("[nvme] PRPLIST read verify FAILED");
    } else {
        g_log->info("[nvme] PRPLIST read verify OK (LBA0 sig + LBA1-31 zero)");
    }
}

/* 写 → FLUSH → 读回逐字节校验。sectors=16 走 PRP2 直连(2页),
 * sectors=32 走 PRP list(4页)。pattern 为首 8 字节 "NVMEWRIT" + 全缓冲
 * 确定式伪随机，重跑幂等（与上次写入内容一致）。 */
static void nvme_selftest_rw(u64 lba, u32 sectors) {
    static u8 wpat[16384];
    static u8 rbuf[16384];
    u32 bytes = sectors * (u32)g_sector_size;
    if (bytes > sizeof(wpat)) return;

    for (u32 i = 0; i < bytes; i++) wpat[i] = (u8)(i * 31u + 17u);
    wpat[0] = 'N'; wpat[1] = 'V'; wpat[2] = 'M'; wpat[3] = 'E';
    wpat[4] = 'W'; wpat[5] = 'R'; wpat[6] = 'I'; wpat[7] = 'T';

    log_hex("[nvme] selftest rw lba=", lba);
    log_hex("[nvme] selftest rw sectors=", sectors);
    int rc = nvme_block_write(0, lba, sectors, wpat);
    log_hex("[nvme]   write+flush rc=", (u64)(i64)rc);
    if (rc != 0) return;

    for (u32 i = 0; i < bytes; i++) rbuf[i] = 0;
    rc = nvme_block_read(0, lba, sectors, rbuf);
    log_hex("[nvme]   readback rc=", (u64)(i64)rc);
    if (rc != 0) return;

    for (u32 i = 0; i < bytes; i++) {
        if (rbuf[i] != wpat[i]) {
            g_log->error("[nvme]   readback MISMATCH");
            log_hex("[nvme]   first bad idx=", i);
            log_hex("[nvme]   expect=", wpat[i]);
            log_hex("[nvme]   actual=", rbuf[i]);
            return;
        }
    }
    g_log->info("[nvme]   write+flush+readback verify OK");
}

/* ---- 真机诊断: dump 关键寄存器 ---- */

static void nvme_dump_regs(const char *ctx) {
    g_log->info("[nvme] --- register dump: ");
    g_log->info(ctx);
    log_hex("[nvme]   CAP  =", mmio_read64(NVME_CAP));
    log_hex("[nvme]   VS   =", mmio_read32(NVME_VS));
    log_hex("[nvme]   CC   =", mmio_read32(NVME_CC));
    log_hex("[nvme]   CSTS =", mmio_read32(NVME_CSTS));
    log_hex("[nvme]   AQA  =", mmio_read32(NVME_AQA));
    log_hex("[nvme]   ASQ  =", mmio_read64(NVME_ASQ));
    log_hex("[nvme]   ACQ  =", mmio_read64(NVME_ACQ));
    /* doorbell 区域首个 entry 诊断 */
    log_hex("[nvme]   SQ0DB=", mmio_read32(NVME_DB_BASE));
    log_hex("[nvme]   CQ0DB=", mmio_read32(NVME_DB_BASE + g_db_stride));
}

/* ---- 高位 BAR MMIO 接入 ---- */

/* 真机 NVMe 控制器寄存器 + doorbell 空间可能远大于 8K。
 * 典型实机: CAP.MQES 决定队列深度，doorbell 偏移 = DB_BASE + 2*QID*DSTRD*4，
 * 需要映射足够大的范围覆盖至少 admin + 1 个 IO queue 对。
 * 安全映射大小: 至少 64K（覆盖 QID 0-7 的 SQ/CQ doorbell）。 */
#define NVME_MAP_SIZE  0x10000

static volatile u8 *nvme_map_bar(const struct dkm_kernel_api *api, u64 bar_phys) {
    u64 bar_virt = api->hhdm_offset + bar_phys;

    /* 优先使用 mm_map_mmio（独立窗口映射, PCD|PWT, 不与 HHDM 空洞竞争） */
    if (api->mm_map_mmio) {
        void *mapped = api->mm_map_mmio(bar_phys, NVME_MAP_SIZE);
        if (mapped) {
            g_log->info("[nvme] BAR mapped via mm_map_mmio window");
            return (volatile u8 *)mapped;
        }
        g_log->warn("[nvme] mm_map_mmio failed; trying HHDM fallback");
    }

    if (!api->mmio || !api->mmio->is_mapped) {
        /* 内核无页表服务: 仅当 BAR 位于低 4G（HHDM 经验上覆盖）时冒险直用 */
        if (bar_phys < 0x100000000ULL) {
            g_log->warn("[nvme] no mmio api; assume HHDM covers low BAR");
            return (volatile u8 *)(uintptr_t)bar_virt;
        }
        g_log->warn("[nvme] no mmio api and BAR above 4G; cannot access");
        return (volatile u8 *)0;
    }

    log_hex("[nvme] CR3=", api->mmio->cr3 ? api->mmio->cr3() : 0);

    /* 核实: HHDM 是否直接覆盖该 BAR（四级页表只读 walk, 无 #PF 风险） */
    int level = api->mmio->is_mapped(bar_virt);
    log_hex("[nvme] HHDM is_mapped(BAR)=", (u64)(i64)level);

    if (level == 0) {
        g_log->info("[nvme] HHDM does NOT cover BAR; mapping via page tables");
        if (!api->mmio->map_mmio) return (volatile u8 *)0;
        int rc = api->mmio->map_mmio(bar_phys, NVME_MAP_SIZE);
        log_hex("[nvme] map_mmio rc=", (u64)(i64)rc);
        if (rc != 0) return (volatile u8 *)0;

        level = api->mmio->is_mapped(bar_virt);
        log_hex("[nvme] post-map is_mapped=", (u64)(i64)level);
        if (level == 0) {
            g_log->error("[nvme] BAR still unmapped after map_mmio");
            return (volatile u8 *)0;
        }
    } else {
        g_log->info("[nvme] HHDM already covers BAR; direct access");
    }

    return (volatile u8 *)(uintptr_t)bar_virt;
}

/* ---- driver entry ---- */

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    dkm_tsc_calibrate();

    if (!api || !api->log) return -1;
    g_log = api->log;
    dkm_instr_init(api);

    g_log->info("[nvme] init begin");

    u8 bus = 0, dev = 0, func = 0;
    if (nvme_find(&bus, &dev, &func) != 0) {
        g_log->warn("[nvme] NVMe controller not found");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    g_log->info("[nvme] NVMe controller found");
    DKM_STAT_INC(nvm0_pci_found);
    log_hex("[nvme] bus=", bus);
    log_hex("[nvme] dev=", dev);
    log_hex("[nvme] func=", func);

    u32 vd = dkm_pci_read(bus, dev, func, PCI_VENDOR_ID);
    log_hex("[nvme] vendor=", vd & 0xffff);
    log_hex("[nvme] device=", vd >> 16);

    /* 启用 MEM + BUSM, 并禁用 INTx（轮询模式不需要中断） */
    u32 command = dkm_pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_MEM | PCI_CMD_BUSM | (1u << 10);
    dkm_pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[nvme] PCI command=", command);

    u32 bar0_lo = dkm_pci_read(bus, dev, func, PCI_BAR0);
    u32 bar1_hi = dkm_pci_read(bus, dev, func, PCI_BAR1);
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

    u32 irq_line = dkm_pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[nvme] BAR0 phys=", bar_phys);
    log_hex("[nvme] PCI IRQ line=", irq_line);
    log_hex("[nvme] HHDM offset=", api->hhdm_offset);

    if (!api->hhdm_offset || !bar_phys) {
        g_log->warn("[nvme] missing HHDM or BAR0; skip");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    g_regs = nvme_map_bar(api, bar_phys);
    if (!g_regs) {
        g_log->error("[nvme] BAR MMIO unavailable; driver inactive");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    /* ---- 寄存器枚举（验证 MMIO 可读） ---- */
    u64 cap = mmio_read64(NVME_CAP);
    u32 vs  = mmio_read32(NVME_VS);
    u32 cc  = mmio_read32(NVME_CC);
    u32 csts = mmio_read32(NVME_CSTS);

    log_hex("[nvme] CAP=", cap);
    log_hex("[nvme] VS=", vs);
    log_hex("[nvme] CC=", cc);
    log_hex("[nvme] CSTS=", csts);
    log_hex("[nvme] CAP.MQES=", (cap & 0xffff) + 1);
    log_hex("[nvme] CAP.DSTRD=", (cap >> 32) & 0xf);
    log_hex("[nvme] CAP.TO=", (cap >> 24) & 0xff);
    log_hex("[nvme] CAP.CSS=", (cap >> 37) & 0xff);
    log_hex("[nvme] CAP.MPSMIN=", (cap >> 48) & 0xf);
    log_hex("[nvme] CAP.MPSMAX=", (cap >> 52) & 0xf);

    if (cap == 0 || cap == 0xFFFFFFFFFFFFFFFFULL) {
        g_log->error("[nvme] CAP invalid; MMIO read broken");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    if (!((cap >> 37) & 1u)) {
        g_log->error("[nvme] NVM command set not supported");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    g_db_stride = 4u << ((cap >> 32) & 0xf);
    g_timeout_ms = ((cap >> 24) & 0xff) * 500;
    if (!g_timeout_ms) g_timeout_ms = 500;
    log_hex("[nvme] doorbell stride=", g_db_stride);
    log_hex("[nvme] timeout ms=", g_timeout_ms);

    if (!api->dma) {
        g_log->warn("[nvme] DMA API unavailable; queues not configured");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    /* ---- 分配队列与数据缓冲（低 4G, 4K 对齐, 清零, 物理连续） ---- */
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_asq) != 0) goto dma_fail;
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_acq) != 0) goto dma_fail;
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_ident) != 0) goto dma_fail;
    if (api->dma->alloc_pages(NVME_DATA_PAGES, 4096, 0x100000000ULL, &g_data) != 0) goto dma_fail;
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_prp) != 0) goto dma_fail;
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_iosq) != 0) goto dma_fail;
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_iocq) != 0) goto dma_fail;
    goto dma_ok;
dma_fail:
    g_log->error("[nvme] DMA alloc failed");
    g_log->info("[nvme] driver ready");
    return 0;
dma_ok:
    log_hex("[nvme] ASQ phys=", g_asq.phys);
    log_hex("[nvme] ACQ phys=", g_acq.phys);
    log_hex("[nvme] DATA phys=", g_data.phys);
    log_hex("[nvme] PRP list phys=", g_prp.phys);
    log_hex("[nvme] IOSQ phys=", g_iosq.phys);
    log_hex("[nvme] IOCQ phys=", g_iocq.phys);

    /* ---- 控制器复位: EN=0 等 RDY=0 ---- */
    mmio_write32(NVME_INTMS, 0xFFFFFFFFu);        /* 屏蔽全部中断向量 */
    cc = mmio_read32(NVME_CC);
    if (cc & 1u) {
        mmio_write32(NVME_CC, cc & ~1u);
    }
    if (nvme_wait_csts(1u, 0u, g_timeout_ms) != 0) {
        g_log->error("[nvme] disable failed");
        nvme_dump_regs("controller disable timeout");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    g_log->info("[nvme] controller disabled");

    /* ---- 配置 admin queue ---- */
    mmio_write32(NVME_AQA, ((u32)(ADMIN_QSIZE - 1) << 16) | (u32)(ADMIN_QSIZE - 1));
    mmio_write64(NVME_ASQ, g_asq.phys);
    mmio_write64(NVME_ACQ, g_acq.phys);
    g_asq_tail = 0;
    g_acq_head = 0;
    g_acq_phase = 1;
    g_cid = 1;

    /* ---- 使能: EN=1, CSS=NVM, MPS=4K, AMS=RR, IOSQES=6, IOCQES=4 ---- */
    mmio_write32(NVME_CC, 0x00460001u);
    if (nvme_wait_csts(1u, 1u, g_timeout_ms) != 0) {
        g_log->error("[nvme] enable failed");
        nvme_dump_regs("controller enable timeout");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    g_log->info("[nvme] controller enabled, admin queue live");

    /* ---- Identify ---- */
    u32 nn = 0;
    if (nvme_identify_controller(&nn) != 0) {
        g_log->error("[nvme] identify controller failed");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    if (nn == 0) {
        g_log->warn("[nvme] no namespace");
        g_log->info("[nvme] driver ready");
        return 0;
    }

    u64 nsze = 0;
    u32 lbads = 0;
    if (nvme_identify_namespace(1, &nsze, &lbads) != 0) {
        g_log->error("[nvme] identify namespace failed");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    if (lbads < 9 || lbads > 12 || nsze == 0) {
        g_log->warn("[nvme] unsupported LBA format or empty NS");
        log_hex("[nvme] LBADS=", lbads);
        g_log->info("[nvme] driver ready");
        return 0;
    }
    g_sector_size = 1ULL << lbads;
    g_sector_count = nsze;
    log_hex("[nvme] sector size=", g_sector_size);
    log_hex("[nvme] sector count=", g_sector_count);

    /* ---- 单命令扇区上限: min(bounce 缓冲, 2^MDTS 页, NLB 16-bit) ----
     * QEMU MDTS=7 → 512KiB, bounce 32 页=128KiB 更紧, 故 chunk=256 扇区,
     * 恰好覆盖 DSK FAT32 的 256 扇区批量读（一次命令读满）。 */
    {
        u64 buf_bytes = (u64)NVME_DATA_PAGES * 4096u;
        u64 mdts_bytes = g_mdts ? ((1ULL << g_mdts) * 4096u) : buf_bytes;
        u64 cap = buf_bytes < mdts_bytes ? buf_bytes : mdts_bytes;
        g_chunk_max_sectors = (u32)(cap / g_sector_size);
        if (g_chunk_max_sectors > 65536u) g_chunk_max_sectors = 65536u;
        if (g_chunk_max_sectors == 0) g_chunk_max_sectors = 1;
        log_hex("[nvme] chunk max sectors=", g_chunk_max_sectors);
    }

    /* ---- IO queue ---- */
    if (nvme_create_io_queues() != 0) {
        g_log->error("[nvme] IO queue creation failed");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    g_io_ready = 1;
    g_log->info("[nvme] IO queue live");

    /* ---- LBA0 读取验证 ---- */
    {
        int rc = nvme_io_rw(NVME_IO_READ, 0, 1, g_data.phys);
        log_hex("[nvme] READ LBA0 rc=", (u64)(i64)rc);
        if (rc == 0) {
            u8 *d = (u8 *)g_data.virt;
            u32 *w = (u32 *)g_data.virt;
            log_hex("[nvme] LBA0 dword0=", w[0]);
            log_hex("[nvme] LBA0 dword1=", w[1]);
            log_hex("[nvme] LBA0 dword2=", w[2]);
            log_hex("[nvme] LBA0 dword3=", w[3]);
            static const char sig[] = "DESHABNVME0";
            u32 match = 1;
            for (u32 i = 0; i < 11; i++) {
                if (d[i] != (u8)sig[i]) { match = 0; break; }
            }
            if (match) {
                g_log->info("[nvme] LBA0 signature OK (DESHABNVME0)");
                g_test_disk = 1;
            } else {
                g_log->info("[nvme] LBA0 data read (no test signature)");
            }
        }
    }

    /* ---- 注册 block provider（读 + 写） ---- */
    if (api->block && api->block->register_device) {
        struct dkm_block_device_desc desc;
        desc.name = "nvme0";
        desc.sector_size = g_sector_size;
        desc.sector_count = g_sector_count;
        desc.ctx = 0;
        desc.read = nvme_block_read;
        desc.write = nvme_block_write;
        int index = api->block->register_device(&desc);
        log_hex("[nvme] block provider index=", (u64)(i64)index);
    } else {
        g_log->warn("[nvme] block API unavailable; provider not registered");
    }

    /* ---- 数据面自测: 写测试仅在签名测试盘上运行（防实机盘数据损坏） ---- */
    if (g_test_disk) {
        nvme_selftest_read_multipage();     /* 32 扇区 16KiB: PRP list 读 */
        nvme_selftest_rw(1000, 16);         /* 8KiB: PRP2 直连 (2 页) 写+读回 */
        nvme_selftest_rw(2000, 32);         /* 16KiB: PRP list (4 页) 写+读回 */
        if (nvme_lba0_sig_ok()) {
            g_log->info("[nvme] LBA0 sig intact after write tests");
        } else {
            g_log->error("[nvme] LBA0 sig CORRUPTED after write tests");
        }
    } else {
        g_log->info("[nvme] not a test disk; write self-test skipped");
    }

    g_log->info("[nvme] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
