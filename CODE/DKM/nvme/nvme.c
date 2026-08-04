/* DKM NVMe Driver — 高位 BAR MMIO 映射 + admin/IO queue + block provider
 *
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 *
 * 实现要点:
 *   1. 64-bit BAR 常位于 4G 以上（QEMU/OVMF 把 NVMe BAR0 放到 ~48GiB），
 *      HHDM 直映射不保证覆盖。访问前先用 kernel_api.mmio->is_mapped 做
 *      四级页表 walk 核实；未映射则 map_mmio 原地补建 4KiB UC 页。
 *   2. 完成模型: 单 outstanding 命令 + CQE phase bit。B7 阶段3 起 IO 队列
 *      默认尝试 MSI/MSI-X 中断驱动（FUCK [drivers] nvme_msi, 默认 1 自动）:
 *      先 MSI cap 编程, 缺则 MSI-X (QEMU nvme 只暴露 MSI-X) — 表项
 *      addr=0xFEE00000|dest<<12, data=动态向量, entry unmask + cap Enable;
 *      Create IOCQ IEN=1/IV=0 + INTMC 解除 IV0; handler 摘 CQE 进完成槽、
 *      推进 head、写 CQ doorbell, 自写 LAPIC EOI 并返回 1 (MSI 直投 LAPIC,
 *      与 apic_route 开关无关, 不依赖阶段2 的 EOI 钩子注册状态)。
 *      提交侧统一等待: 先查完成槽(中断路径), 再在 irqsave 临界区内手动
 *      摘取(自适应轮询路径) — IF=0 (DSK 阶段) 或 MSI 未投递时零感知降级,
 *      行为与旧版纯轮询一致。计数: g_msi_irq_count / g_msi_slot_hits /
 *      g_msi_poll_harvests / g_msi_timeouts, init 末尾与压测后打点。
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
    .version        = "0.4.0",
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

/* ---- B7 阶段3: MSI 状态 ---- */
static int g_cfg_nvme_msi = 1;      /* FUCK [drivers] nvme_msi, 默认 1 自动尝试 */
static int g_msi_enabled;           /* MSI/MSI-X 编程成功且 handler 已注册 */
static int g_msix_mode;             /* 1=MSI-X 表项模式, 0=MSI cap 模式 */
static int g_msi_vector = -1;       /* 动态分配的 IDT 向量 (0x40-0xDF 池) */
/* 完成槽: 单 outstanding 前提下, handler 是唯一写者、提交侧是唯一读者,
 * 且下一命令提交前不可能再有新 CQE, 故读-清无需互斥; valid 最后写/最先读,
 * x86 TSO  store-store / load-load 天然有序, nvme_mb 只挡编译器重排。 */
static volatile u32 g_slot_valid;
static volatile u32 g_slot_status;  /* sc | (sct << 8), 0 = 成功 */
static volatile u32 g_slot_dw0;     /* CQE dw0 (命令特定返回) */
static volatile u32 g_slot_cid;     /* CQE dw3 低 16 位 CID, 提交侧比对防错位 */
static u64 g_msi_irq_count;         /* MSI handler 进入次数 (投递证明) */
static u64 g_msi_slot_hits;         /* 提交侧经完成槽完成的命令数 (中断路径) */
static u64 g_msi_poll_harvests;     /* 提交侧 irqsave 手动摘取数 (轮询路径) */
static u64 g_msi_timeouts;          /* 完成超时计数 (真故障, 非降级) */
static u64 g_msi_cid_mismatch;      /* 摘到 CID 与提交 cid 不符的 CQE 计数 (队列错位警报) */

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

/* 十进制打印 (MSI 计数器打点用, 比 hex 直观) */
static void log_dec(const char *prefix, u64 val) {
    char buf[24];
    u32 pos = 0;
    if (!val) {
        buf[pos++] = '0';
    } else {
        char tmp[20];
        u32 n = 0;
        while (val) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
        while (n) buf[pos++] = tmp[--n];
    }
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static int memeq(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static u32 str_len(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

/* 在 NUL 结尾字符串中查找子串 (boot module cmdline 匹配用) */
static int str_contains(const char *hay, const char *needle) {
    u32 nlen = str_len(needle);
    if (!nlen) return 1;
    for (u32 i = 0; hay[i]; i++) {
        u32 j = 0;
        while (j < nlen && hay[i + j] && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static void nvme_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

/* irqsave 临界区: 提交侧手动摘取 CQE 时屏蔽中断, 与 MSI handler 互斥。
 * pushf/popf 配对恢复 IF — DSK 阶段 IF=0, 绝不能无条件 sti。 */
static __inline__ u64 nvme_irqsave(void) {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static __inline__ void nvme_irqrestore(u64 flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
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

/* ---- FUCK 配置自解析 (与 apic.c 同一范式, 不依赖 UTSM ini_parser) ----
 * UTSM 经 Limine boot module (cmdline="fuck:config") 预加载 FUCK;
 * kernel_api.boot_modules_response 对所有 DKM 驱动可见。
 * 此处只找 [drivers] 分区下的 nvme_msi 键; 缺失文件/键一律默认 1
 * (自动尝试 MSI, 失败回轮询), 与无配置时行为一致。
 */

/* Limine boot module 最小类型 (与 apic.c / bootfs.c / UTSM limine.h 布局一致) */
struct limine_file_min {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u32 media_type;
    u32 unused;
    u32 tftp_ip;
    u32 tftp_port;
    u32 partition_index;
    u32 mbr_disk_id;
    u8 gpt_disk_uuid[16];
    u8 gpt_part_uuid[16];
    u8 part_uuid[16];
};

struct limine_module_response_min {
    u64 revision;
    u64 module_count;
    struct limine_file_min **modules;
};

/* 解析值 token: 支持 0/1/true/false/on/off/yes/no 及十进制整数 */
static int parse_scalar(const char *line, u64 start, u64 line_len, int defval) {
    u64 i = start;
    while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
    u64 tok_start = i;
    while (i < line_len && line[i] != ' ' && line[i] != '\t' &&
           line[i] != '#' && line[i] != ';' && line[i] != '\r') i++;
    u64 tok_len = i - tok_start;
    const char *tok = line + tok_start;
    if (!tok_len) return defval;
    if (tok_len == 1 && tok[0] == '1') return 1;
    if (tok_len == 1 && tok[0] == '0') return 0;
    if (tok_len == 4 && memeq(tok, "true", 4)) return 1;
    if (tok_len == 5 && memeq(tok, "false", 5)) return 0;
    if (tok_len == 2 && memeq(tok, "on", 2)) return 1;
    if (tok_len == 3 && memeq(tok, "off", 3)) return 0;
    if (tok_len == 3 && memeq(tok, "yes", 3)) return 1;
    if (tok_len == 2 && memeq(tok, "no", 2)) return 0;
    int neg = 0;
    u64 j = 0;
    if (tok[0] == '-') { neg = 1; j = 1; }
    int val = 0;
    int any = 0;
    for (; j < tok_len; j++) {
        if (tok[j] < '0' || tok[j] > '9') return defval;
        val = val * 10 + (tok[j] - '0');
        any = 1;
    }
    if (!any) return defval;
    return neg ? -val : val;
}

static int fuck_get_int(const char *text, u64 size,
                        const char *section, const char *key, int defval) {
    u64 pos = 0;
    int in_section = 0;
    u32 slen = str_len(section);
    u32 klen = str_len(key);

    while (pos < size) {
        u64 end = pos;
        while (end < size && text[end] != '\n') end++;
        u64 line_len = end - pos;
        if (line_len > 0 && text[pos + line_len - 1] == '\r') line_len--;
        const char *line = text + pos;

        u64 i = 0;
        while (i < line_len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < line_len) {
            char c = line[i];
            if (c == '#' || c == ';') {
                /* 注释行, 跳过 */
            } else if (c == '[') {
                in_section = 0;
                if (line_len >= (u64)(i + 1 + slen + 1) &&
                    memeq(line + i + 1, section, slen) &&
                    line[i + 1 + slen] == ']') {
                    in_section = 1;
                }
            } else if (in_section) {
                if (line_len >= (u64)(i + klen) &&
                    memeq(line + i, key, klen)) {
                    u64 j = i + klen;
                    while (j < line_len && (line[j] == ' ' || line[j] == '\t')) j++;
                    if (j < line_len && line[j] == '=') {
                        return parse_scalar(line, j + 1, line_len, defval);
                    }
                }
            }
        }
        pos = end + 1;
    }
    return defval;
}

static void nvme_load_config(const struct dkm_kernel_api *api) {
    /* 编译期默认: 自动尝试 MSI (任何前提缺失都会回退纯轮询, 零回归) */
    g_cfg_nvme_msi = 1;

    const struct limine_module_response_min *resp =
        (const struct limine_module_response_min *)api->boot_modules_response;
    if (!resp || !resp->modules) {
        g_log->warn("[nvme] no boot modules; FUCK defaults in effect");
        return;
    }

    for (u64 i = 0; i < resp->module_count; i++) {
        struct limine_file_min *f = resp->modules[i];
        if (!f || !f->cmdline || !f->address || !f->size) continue;
        if (!str_contains(f->cmdline, "fuck:config")) continue;

        g_cfg_nvme_msi = fuck_get_int((const char *)f->address, f->size,
                                      "drivers", "nvme_msi", 1);
        g_log->info("[nvme] FUCK config module found");
        log_dec("[nvme] cfg nvme_msi=", (u64)g_cfg_nvme_msi);
        return;
    }

    g_log->warn("[nvme] FUCK module (fuck:config) not found; defaults in effect");
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

/* ---- B7 阶段3: IO CQ 完成摘取 (MSI handler 与提交侧轮询共用) ----
 * 返回 1 = 摘到一枚 CQE; 0 = 无新完成。
 * out_cid 返回 CQE dw3 低 16 位的命令 ID, 供调用方与提交 cid 比对 —
 * 单 outstanding 模型下 CQ 任何时刻只应有当前命令的一枚 CQE, CID 不匹配
 * 即队列状态错位的硬证据, 必须显式报错而非静默当作当前命令完成
 * (route=0 曾观测 stress readback MISMATCH @0x9000, 疑 CQE 提前消费)。
 * 并发模型: 单生产者(控制器 DMA 写 CQ) + 单逻辑消费者, 消费者有两个入口 ——
 *   1. MSI handler (IRQ 上下文): 同一 vector 在 EOI 前不会被 LAPIC 再次
 *      投递, handler 对同向量天然不可重入, 无需自锁;
 *   2. 提交侧统一等待 (主上下文): 必须在 nvme_irqsave 临界区内调用,
 *      屏蔽中断期间 handler 不可能插入, 消除 "phase 判定后、head 推进前
 *      被 handler 抢摘同一 CQE" 的双消费竞态。
 * head 推进 + CQ doorbell 只在此函数内发生, 是两个入口的唯一汇合点。
 */
static int nvme_iocq_harvest(u32 *out_status, u32 *out_dw0, u32 *out_cid) {
    volatile u32 *cq = (volatile u32 *)g_iocq.virt;
    u32 dw3 = cq[g_iocq_head * 4 + 3];
    if (((dw3 >> 16) & 1u) != g_iocq_phase) return 0;

    if (out_dw0) *out_dw0 = cq[g_iocq_head * 4 + 0];
    if (out_cid) *out_cid = dw3 & 0xffffu;
    u32 sc  = (dw3 >> 17) & 0xffu;
    u32 sct = (dw3 >> 25) & 0x7u;
    /* SC 与 SCT 联合编码 (与 nvme_submit 经典路径同一约定):
     * SCT!=0 且 SC==0 时只返回 sc 会被误判成功。 */
    *out_status = sc | (sct << 8);

    g_iocq_head = (g_iocq_head + 1) % IO_QSIZE;
    if (g_iocq_head == 0) g_iocq_phase ^= 1u;
    nvme_mb();                                  /* head 先于 doorbell 可见 */
    mmio_write32(cq_head_db(1), g_iocq_head);
    return 1;
}

/* MSI 中断 handler: 摘 CQE 进完成槽。
 * 返回 1 = 自负 LAPIC EOI (抑制 idt_handler 的钩子补 EOI)。
 * 为何自写 EOI 而不依赖钩子: MSI 是设备向 LAPIC 直投的消息中断,
 * 投递路径不经过 IOAPIC/PIC, 与 apic_route 开关无关; 但 EOI 钩子只在
 * route=1 时注册, route=0 下钩子为 NULL 会漏写 EOI — LAPIC ISR 位
 * 残留将永久阻塞同优先级后续中断, MSI 静默退化为 one-shot。
 * handler 内直写 LAPIC EOI 寄存器后返回 1, 两种路由模式下行为一致。
 * LAPIC 未软件使能时该写为无害 no-op (此时 MSI 本就不会投递)。
 * 地址取 x86 默认 LAPIC base 0xFEE00000 + EOI 偏移 0xB0 (QEMU/MADT
 * 一致; SAS-R0 恒等映射覆盖 4G 以下, apic.drv 已实证可直访)。
 * 时序说明: 即使提交侧轮询抢先摘走 CQE, 控制器中断信号早已发出,
 * 本次 handler 仍会执行并计入 g_msi_irq_count — 投递证据不受竞态影响。 */
static int nvme_msi_handler(u8 vector) {
    (void)vector;
    g_msi_irq_count++;
    u32 st = 0, dw0 = 0, cid = 0;
    if (nvme_iocq_harvest(&st, &dw0, &cid)) {
        g_slot_status = st;
        g_slot_dw0 = dw0;
        g_slot_cid = cid;
        nvme_mb();              /* payload 先于 valid 对提交侧可见 */
        g_slot_valid = 1;
    }
    *(volatile u32 *)(uintptr_t)0xFEE000B0u = 0;    /* LAPIC EOI */
    return 1;
}

/* IO 命令统一完成等待 (MSI 模式): 完成槽(中断路径)优先, irqsave 手动摘取
 * (自适应轮询路径)兜底。MSI 健康时 handler 在 CQE 落盘瞬间填槽; MSI 未投递
 * (LAPIC 未启用/IF=0/信号丢失) 时手动摘取沿原 phase 语义直接完成, 调用方
 * 零感知。超时语义与经典路径一致 (CAP.TO*500ms*10, 下限 5s)。 */
static int nvme_io_wait_completion(u16 cid, u32 opcode, u32 *out_cqe_dw0) {
    u64 submit_timeout_ms = g_timeout_ms * 10;
    if (submit_timeout_ms < 5000) submit_timeout_ms = 5000;
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * submit_timeout_ms;

    for (;;) {
        /* 中断路径: handler 已摘取 CQE 进完成槽 */
        if (g_slot_valid) {
            u32 st = g_slot_status;
            u32 dw0 = g_slot_dw0;
            u32 scid = g_slot_cid;
            nvme_mb();
            g_slot_valid = 0;
            g_msi_slot_hits++;
            if (scid != (u32)cid) {
                /* CID 错位: 槽内 CQE 不属于本命令 — 队列状态已损,
                 * 显式报错并继续等真 CQE (超时兜底), 绝不静默当成功 */
                g_msi_cid_mismatch++;
                g_log->error("[nvme] CID mismatch (slot)");
                log_hex("[nvme] expect cid=", cid);
                log_hex("[nvme] got cid=", scid);
                continue;
            }
            if (out_cqe_dw0) *out_cqe_dw0 = dw0;
            if (st) {
                g_log->error("[nvme] command failed (msi slot)");
                log_hex("[nvme] opcode=", opcode);
                log_hex("[nvme] cid=", cid);
                log_hex("[nvme] status=", st);
                return (int)st;
            }
            return 0;
        }

        /* 自适应轮询路径: irqsave 临界区内手动摘取 (与 handler 互斥) */
        u32 st = 0, dw0 = 0, hcid = 0;
        u64 flags = nvme_irqsave();
        int got = nvme_iocq_harvest(&st, &dw0, &hcid);
        nvme_irqrestore(flags);
        if (got) {
            g_msi_poll_harvests++;
            if (hcid != (u32)cid) {
                g_msi_cid_mismatch++;
                g_log->error("[nvme] CID mismatch (poll)");
                log_hex("[nvme] expect cid=", cid);
                log_hex("[nvme] got cid=", hcid);
                continue;
            }
            if (out_cqe_dw0) *out_cqe_dw0 = dw0;
            if (st) {
                g_log->error("[nvme] command failed");
                log_hex("[nvme] opcode=", opcode);
                log_hex("[nvme] cid=", cid);
                log_hex("[nvme] status=", st);
                return (int)st;
            }
            return 0;
        }

        if (dkm_rdtsc() > deadline) {
            g_msi_timeouts++;
            g_log->error("[nvme] completion timeout (msi mode)");
            log_hex("[nvme] opcode=", opcode);
            log_hex("[nvme] cid=", cid);
            nvme_dump_regs("completion timeout");
            return -100;
        }
        __asm__ volatile("pause");
    }
}

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
    /* CID=0 保留不用: QEMU 在 MSI-X unmask 边界会向 IO CQ 补投一枚
     * CID=0/phase 正确/status 成功的幽灵 CQE (实测稳定复现), g_cid 回绕
     * 跳过 0 可保证该幽灵 CQE 永远被 CID 校验拒绝, 绝无误判窗口。 */
    if (g_cid == 0) g_cid = 1;

    /* 逐 dword 写入 SQE（64B），避免 freestanding 下编译器生成 memcpy 调用 */
    u32 pos = *sq_tail;
    for (u32 i = 0; i < 16; i++) sq[pos * 16 + i] = dw[i];

    *sq_tail = (pos + 1) % qsize;
    nvme_mb();                                  /* SQ 内容先于 doorbell 可见 */
    mmio_write32(sq_tail_db(sq_qid), *sq_tail);

    /* B7 阶段3: IO 队列 MSI 模式 → 完成槽 + irqsave 摘取统一等待。
     * admin 队列保持纯轮询 (仅 init 期使用, 且 INTMC 解除前 IV0 处于
     * 屏蔽态, admin 完成不会触发 MSI)。 */
    if (cq_qid == 1 && g_msi_enabled) {
        return nvme_io_wait_completion(cid, dw[0] & 0xffu, out_cqe_dw0);
    }

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

/* ---- B7 阶段3: PCI MSI 能力编程 ----
 * MSI cap (ID 0x05) 布局:
 *   +0x00 u8  Cap ID (=0x05)        +0x01 u8  Next Ptr
 *   +0x02 u16 Message Control: bit0 Enable, bit[3:1] MMC(可申请向量数-1),
 *              bit[6:4] MME(已分配向量数-1), bit7 64-bit Capable, bit8 PVM
 *   +0x04 u32 Message Address
 *   64-bit 时: +0x08 u32 Message Upper Address, +0x0C u16 Message Data
 *   32-bit 时: +0x08 u16 Message Data
 *   PVM(bit8)=1 时另有 Mask/Pending 寄存器, 复位值 0 (全部 unmask)。
 * x86 LAPIC 投递公式 (physical destination, fixed delivery):
 *   Address = 0xFEE00000 | (dest_apic_id << 12)   (RH=0, DM=0)
 *   Data    = vector | (0<<8 delivery=fixed)      (level/trigger 对 MSI 无效)
 */
#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_MSIX  0x11
#define PCI_STATUS_CAPLIST 0x10u

static int nvme_find_pci_cap(u8 bus, u8 dev, u8 func, u8 cap_id) {
    u32 reg04 = dkm_pci_read(bus, dev, func, PCI_COMMAND);
    if (!((reg04 >> 16) & PCI_STATUS_CAPLIST)) return -1;   /* Status.CapList=0 */
    u8 ptr = (u8)(dkm_pci_read(bus, dev, func, PCI_CAP_PTR) & 0xFC);
    int guard = 0;
    while (ptr && guard++ < 64) {           /* guard 防配置空间坏链死循环 */
        u32 dw = dkm_pci_read(bus, dev, func, ptr);
        u8 id   = (u8)(dw & 0xff);
        u8 next = (u8)((dw >> 8) & 0xff);
        log_hex("[nvme] pci cap id=", id);
        log_hex("[nvme] pci cap at=", ptr);
        if (id == cap_id) return (int)ptr;
        ptr = next & 0xFC;
    }
    return -1;
}

/* MSI cap (0x05) 编程路径: 成功返回 1, cap 缺失或失败返回 0。
 * 失败路径完整回滚 (恢复 msg_ctl + 释放向量), 不留半编程状态。 */
static int nvme_msi_program(const struct dkm_kernel_api *api,
                            u8 bus, u8 dev, u8 func) {
    int cap = nvme_find_pci_cap(bus, dev, func, PCI_CAP_ID_MSI);
    if (cap < 0) {
        g_log->info("[nvme] MSI capability not present");
        return 0;
    }

    u32 cdw = dkm_pci_read(bus, dev, func, (u8)cap);
    u32 msg_ctl = (cdw >> 16) & 0xffffu;
    int cap64 = (int)((msg_ctl >> 7) & 1u);
    int pvm   = (int)((msg_ctl >> 8) & 1u);
    log_hex("[nvme] MSI msg_ctl=", msg_ctl);
    log_hex("[nvme] MSI 64bit=", (u64)cap64);
    log_hex("[nvme] MSI pvm=", (u64)pvm);

    /* 动态向量: UTSM 位图池 0x40-0xDF, 与静态向量 (tick 0xE0/spurious 0xFF)
     * 及 legacy PIC 段 (0x20-0x2F) 无交叠。 */
    int vector = api->irq_vector_alloc();
    if (vector < 0 || vector > 255) {
        g_log->warn("[nvme] MSI: vector alloc failed; polling");
        return 0;
    }
    log_dec("[nvme] MSI vector=", (u64)vector);

    /* 目的 APIC ID: CPUID.1:EBX[31:24] = BSP initial APIC ID (QEMU=0) */
    u32 ca, cb, cc_, cd;
    nvme_cpuid(1, &ca, &cb, &cc_, &cd);
    (void)ca; (void)cc_; (void)cd;
    u32 dest = (cb >> 24) & 0xffu;
    log_hex("[nvme] MSI dest apic id=", dest);

    u32 addr = 0xFEE00000u | (dest << 12);
    u32 data = (u32)vector & 0xffu;         /* fixed delivery, edge */

    /* 先写 Address/Data, 最后置 Enable — 避免半成品消息被发出 */
    dkm_pci_write(bus, dev, func, (u8)(cap + 4), addr);
    if (cap64) {
        dkm_pci_write(bus, dev, func, (u8)(cap + 8), 0);    /* upper addr */
        u32 dw = dkm_pci_read(bus, dev, func, (u8)(cap + 12));
        dw = (dw & 0xffff0000u) | data;                     /* data 为低 u16 */
        dkm_pci_write(bus, dev, func, (u8)(cap + 12), dw);
    } else {
        u32 dw = dkm_pci_read(bus, dev, func, (u8)(cap + 8));
        dw = (dw & 0xffff0000u) | data;
        dkm_pci_write(bus, dev, func, (u8)(cap + 8), dw);
    }

    /* Message Control: MME=0 (单向量), Enable=1; 低 u16 (id/next) 回写原值 */
    u32 mc = msg_ctl;
    mc &= ~(0x7u << 4);                     /* MME=0 → 1 vector */
    mc |= 1u;                               /* MSI Enable */
    cdw = (cdw & 0x0000ffffu) | (mc << 16);
    dkm_pci_write(bus, dev, func, (u8)cap, cdw);

    /* PVM: 显式清 mask bit0 (复位值即为 0, 此处防御性 RMW) */
    if (pvm) {
        int mask_off = cap64 ? cap + 16 : cap + 12;
        u32 mask = dkm_pci_read(bus, dev, func, (u8)mask_off);
        mask &= ~1u;
        dkm_pci_write(bus, dev, func, (u8)mask_off, mask);
        log_hex("[nvme] MSI mask cleared=", mask);
    }

    /* 回读验证: Enable 置位 + Address/Data 与编程值一致 */
    u32 rb_ctl  = (dkm_pci_read(bus, dev, func, (u8)cap) >> 16) & 0xffffu;
    u32 rb_addr = dkm_pci_read(bus, dev, func, (u8)(cap + 4));
    u32 rb_data = dkm_pci_read(bus, dev, func,
                               (u8)(cap64 ? cap + 12 : cap + 8)) & 0xffffu;
    log_hex("[nvme] MSI readback ctl=", rb_ctl);
    log_hex("[nvme] MSI readback addr=", rb_addr);
    log_hex("[nvme] MSI readback data=", rb_data);
    if (!(rb_ctl & 1u) || rb_addr != addr || (rb_data & 0xffu) != data) {
        g_log->warn("[nvme] MSI: readback mismatch; rolling back");
        u32 rb = dkm_pci_read(bus, dev, func, (u8)cap);
        rb = (rb & 0x0000ffffu) | (msg_ctl << 16);      /* 恢复原 msg_ctl */
        dkm_pci_write(bus, dev, func, (u8)cap, rb);
        api->irq_vector_free(vector);
        return 0;
    }

    /* 注册 handler (irq_register 已接受向量命名空间, >0x2F 直挂分发表) */
    api->irq_register((u8)vector, (void *)(uintptr_t)nvme_msi_handler);

    g_msi_enabled = 1;
    g_msi_vector = vector;
    g_log->info("[nvme] MSI enabled (IO CQ IEN=1, IV=0)");
    return 1;
}

/* 按 BIR 读取 BAR 物理基址 (处理 64-bit BAR; IO BAR 返回 0) */
static u64 nvme_bar_phys(u8 bus, u8 dev, u8 func, u32 bir) {
    u8 reg = (u8)(PCI_BAR0 + bir * 4u);
    u32 lo = dkm_pci_read(bus, dev, func, reg);
    if (lo & 1u) return 0;                              /* IO BAR */
    if ((lo & 0x6u) == 0x4u) {                          /* 64-bit MMIO BAR */
        u32 hi = dkm_pci_read(bus, dev, func, (u8)(reg + 4));
        return ((u64)hi << 32) | (u64)(lo & 0xFFFFFFF0u);
    }
    return (u64)(lo & 0xFFFFFFF0u);
}

/* MSI-X cap (0x11) 编程路径: 成功返回 1, cap 缺失或失败返回 0。
 * QEMU nvme 只暴露 MSI-X (无 0x05), 本路径是 QEMU 下中断驱动化的唯一通道。
 * 与 MSI 的差异仅在消息存储位置: MSI-X 的 addr/data 存放在 BAR 内的
 * 表项 (16B/项), 需先把表区 MMIO 映射进 CPU 地址空间再编程 entry 0。
 * 失败路径完整回滚 (清 Enable + 表项重新 mask + 释放向量)。 */
static int nvme_msix_program(const struct dkm_kernel_api *api,
                             u8 bus, u8 dev, u8 func) {
    int cap = nvme_find_pci_cap(bus, dev, func, PCI_CAP_ID_MSIX);
    if (cap < 0) {
        g_log->warn("[nvme] MSI-X capability not present; polling");
        return 0;
    }
    if (!api->mm_map_mmio) {
        g_log->warn("[nvme] MSI-X: mm_map_mmio unavailable; polling");
        return 0;
    }

    u32 cdw = dkm_pci_read(bus, dev, func, (u8)cap);
    u32 msg_ctl = (cdw >> 16) & 0xffffu;
    u32 table_size = (msg_ctl & 0x7ffu) + 1u;           /* N-1 encoded */
    u32 tbl_dw  = dkm_pci_read(bus, dev, func, (u8)(cap + 4));
    u32 bir     = tbl_dw & 7u;
    u32 tbl_off = tbl_dw & ~7u;
    log_hex("[nvme] MSI-X msg_ctl=", msg_ctl);
    log_dec("[nvme] MSI-X table size=", table_size);
    log_hex("[nvme] MSI-X table BIR=", bir);
    log_hex("[nvme] MSI-X table offset=", tbl_off);
    if (bir > 5u) {
        g_log->warn("[nvme] MSI-X: invalid BIR; polling");
        return 0;
    }

    u64 bar_phys = nvme_bar_phys(bus, dev, func, bir);
    if (!bar_phys) {
        g_log->warn("[nvme] MSI-X: table BAR invalid; polling");
        return 0;
    }
    u64 tbl_phys = bar_phys + tbl_off;
    log_hex("[nvme] MSI-X table BAR phys=", bar_phys);
    log_hex("[nvme] MSI-X table phys=", tbl_phys);

    /* 映射表区首页 (含 entry0 的 16B)。mm_map_mmio 返回含页内偏移的指针,
     * 页属性 PCD|PWT (UC), MSI-X 表访问不要求 cache 一致性操作。 */
    volatile u32 *tbl = (volatile u32 *)api->mm_map_mmio(tbl_phys, 4096);
    if (!tbl) {
        g_log->warn("[nvme] MSI-X: table MMIO map failed; polling");
        return 0;
    }

    int vector = api->irq_vector_alloc();
    if (vector < 0 || vector > 255) {
        g_log->warn("[nvme] MSI-X: vector alloc failed; polling");
        return 0;
    }
    log_dec("[nvme] MSI-X vector=", (u64)vector);

    u32 ca, cb, cc_, cd;
    nvme_cpuid(1, &ca, &cb, &cc_, &cd);
    (void)ca; (void)cc_; (void)cd;
    u32 dest = (cb >> 24) & 0xffu;
    u32 addr = 0xFEE00000u | (dest << 12);

    /* 先写表项 (addr/data/mask=0), 再置 cap Enable — 避免半成品消息 */
    volatile u32 *e = tbl;                              /* entry 0 */
    e[0] = addr;                                        /* Msg Addr Lo */
    e[1] = 0;                                           /* Msg Addr Hi */
    e[2] = (u32)vector & 0xffu;                         /* Msg Data = 向量 */
    e[3] = 0;                                           /* Vector Control: unmask */
    nvme_mb();

    u32 nc = (msg_ctl | 0x8000u) & ~(1u << 14);         /* Enable=1, FunctionMask=0 */
    cdw = (cdw & 0x0000ffffu) | (nc << 16);
    dkm_pci_write(bus, dev, func, (u8)cap, cdw);

    /* 回读验证: cap Enable + 表项 addr/data/mask 与编程值一致 */
    u32 rb_ctl = (dkm_pci_read(bus, dev, func, (u8)cap) >> 16) & 0xffffu;
    u32 rb_a0 = e[0], rb_a1 = e[1], rb_d = e[2], rb_vc = e[3];
    log_hex("[nvme] MSI-X readback ctl=", rb_ctl);
    log_hex("[nvme] MSI-X readback entry addr=", ((u64)rb_a1 << 32) | rb_a0);
    log_hex("[nvme] MSI-X readback entry data=", rb_d);
    log_hex("[nvme] MSI-X readback entry vctl=", rb_vc);
    if (!(rb_ctl & 0x8000u) || rb_a0 != addr || rb_a1 != 0 ||
        (rb_d & 0xffu) != ((u32)vector & 0xffu) || (rb_vc & 1u)) {
        g_log->warn("[nvme] MSI-X: readback mismatch; rolling back");
        u32 rb = dkm_pci_read(bus, dev, func, (u8)cap);
        rb = (rb & 0x0000ffffu) | (msg_ctl << 16);      /* 恢复原 msg_ctl */
        dkm_pci_write(bus, dev, func, (u8)cap, rb);
        e[3] = 1;                                       /* 表项重新 mask */
        api->irq_vector_free(vector);
        return 0;
    }

    api->irq_register((u8)vector, (void *)(uintptr_t)nvme_msi_handler);

    g_msi_enabled = 1;
    g_msix_mode = 1;
    g_msi_vector = vector;
    g_log->info("[nvme] MSI-X enabled (table entry0, IO CQ IEN=1, IV=0)");
    return 1;
}

/* 中断驱动化入口: 先 MSI (0x05), 缺则 MSI-X (0x11), 皆不可则纯轮询。
 * 前提检查集中在此; 两条编程路径各自保证失败完整回滚。 */
static int nvme_msi_setup(const struct dkm_kernel_api *api,
                          u8 bus, u8 dev, u8 func) {
    if (!g_cfg_nvme_msi) {
        g_log->info("[nvme] MSI disabled by FUCK (nvme_msi=0); polling mode");
        return 0;
    }
    if (!api->irq_vector_alloc || !api->irq_vector_free || !api->irq_register) {
        g_log->warn("[nvme] MSI: kernel vector allocator unavailable; polling");
        return 0;
    }
    if (nvme_msi_program(api, bus, dev, func)) return 1;
    return nvme_msix_program(api, bus, dev, func);
}

/* ---- IO queue 创建 ---- */

static int nvme_create_io_queues(void) {
    u32 dw[16];

    /* Create IO CQ (qid=1): PC=1; MSI 就绪时 IEN=1 + IV=0
     * CDW10 = QSIZE[31:16](0's based) | QID[15:0] —— QID 必须在低半字
     * CDW11 = IV[31:16] | IEN[1] | PC[0] —— IV=0 与 INTMS/INTMC bit0 对应 */
    nvme_zero(dw, sizeof(dw));
    dw[0] = NVME_ADM_CREATE_IOCQ;
    dw[6] = (u32)(g_iocq.phys & 0xffffffffu);
    dw[7] = (u32)(g_iocq.phys >> 32);
    dw[10] = ((u32)(IO_QSIZE - 1u) << 16) | 1u;
    dw[11] = 1u;
    if (g_msi_enabled) dw[11] = 1u | (1u << 1);     /* PC=1, IEN=1, IV=0 */
    log_hex("[nvme] create IOCQ CDW11=", dw[11]);
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

/* B7 阶段3 压测: 128 扇区 (64KiB, PRP list 路径) × 50 次 写+读。
 * 统计总耗时与 MSI 计数器增量, 供中断/轮询两模式粗粒度对比。
 * 仅签名测试盘运行 (写 LBA3000 起的 50 个块, 不碰 LBA0 签名区)。 */
static void nvme_selftest_stress(void) {
    static u8 wbuf[65536];
    static u8 rbuf[65536];
    for (u32 i = 0; i < sizeof(wbuf); i++) wbuf[i] = (u8)(i * 17u + 3u);

    u64 irq0 = g_msi_irq_count, slot0 = g_msi_slot_hits, poll0 = g_msi_poll_harvests;
    u64 t0 = dkm_rdtsc();
    u32 fails = 0;
    for (u32 i = 0; i < 50; i++) {
        u64 lba = 3000 + (u64)i * 128;
        if (nvme_block_write(0, lba, 128, wbuf) != 0) { fails++; break; }
        if (nvme_block_read(0, lba, 128, rbuf) != 0) { fails++; break; }
    }
    u64 elapsed_ms = dkm_tsc_per_ms ? (dkm_rdtsc() - t0) / dkm_tsc_per_ms : 0;

    /* 末次读回全量校验 (数据通路在 MSI 模式下完整性证明) */
    if (!fails) {
        for (u32 i = 0; i < sizeof(wbuf); i++) {
            if (rbuf[i] != wbuf[i]) {
                g_log->error("[nvme] stress readback MISMATCH");
                log_hex("[nvme]   first bad idx=", i);
                log_hex("[nvme]   expect=", wbuf[i]);
                log_hex("[nvme]   actual=", rbuf[i]);
                log_hex("[nvme]   cq head=", g_iocq_head);
                log_hex("[nvme]   cq phase=", g_iocq_phase);
                log_hex("[nvme]   slot valid=", g_slot_valid);
                fails++;
                break;
            }
        }
    }

    log_dec("[nvme] stress 50x(128sec W+R) ms=", elapsed_ms);
    log_dec("[nvme] stress fails=", fails);
    log_dec("[nvme] stress irq delta=", g_msi_irq_count - irq0);
    log_dec("[nvme] stress slot delta=", g_msi_slot_hits - slot0);
    log_dec("[nvme] stress poll delta=", g_msi_poll_harvests - poll0);
    if (!fails) g_log->info("[nvme] stress verify OK");
}

static void nvme_msi_stats_dump(const char *tag) {
    g_log->info(tag);
    log_dec("[nvme] msi enabled=", (u64)g_msi_enabled);
    log_dec("[nvme] msix mode=", (u64)g_msix_mode);
    log_dec("[nvme] msi vector=", (u64)(g_msi_vector < 0 ? 0 : g_msi_vector));
    log_dec("[nvme] msi irq count=", g_msi_irq_count);
    log_dec("[nvme] msi slot hits=", g_msi_slot_hits);
    log_dec("[nvme] msi poll harvests=", g_msi_poll_harvests);
    log_dec("[nvme] msi timeouts=", g_msi_timeouts);
    log_dec("[nvme] msi cid mismatch=", g_msi_cid_mismatch);
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
    nvme_load_config(api);

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

    /* ---- B7 阶段3: IO queue 创建前尝试 MSI (任何前提缺失回退纯轮询) ---- */
    nvme_msi_setup(api, bus, dev, func);

    /* ---- IO queue ---- */
    if (nvme_create_io_queues() != 0) {
        g_log->error("[nvme] IO queue creation failed");
        g_log->info("[nvme] driver ready");
        return 0;
    }
    g_io_ready = 1;
    g_log->info("[nvme] IO queue live");

    /* MSI 就绪: INTMC 解除 IV0 屏蔽 (此前 IV0 保持屏蔽, admin 完成不产生
     * MSI; 此后 IO CQ 每次完成经 MSI 投递到 g_msi_vector) */
    if (g_msi_enabled) {
        mmio_write32(NVME_INTMC, 1u);
        log_hex("[nvme] INTMC unmask IV0, INTMS=", mmio_read32(NVME_INTMS));
    }

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
        nvme_selftest_stress();             /* B7: 128 扇区×50 压测 + MSI 计数 */
        if (nvme_lba0_sig_ok()) {
            g_log->info("[nvme] LBA0 sig intact after write tests");
        } else {
            g_log->error("[nvme] LBA0 sig CORRUPTED after write tests");
        }
    } else {
        g_log->info("[nvme] not a test disk; write self-test skipped");
    }

    nvme_msi_stats_dump("[nvme] --- MSI stats ---");

    g_log->info("[nvme] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
