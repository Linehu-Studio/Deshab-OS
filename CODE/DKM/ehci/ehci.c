/* DKM EHCI Driver — USB2 host controller + Mass Storage Class (BOT/SCSI)
 *
 * Stage 1 optional storage driver, depends on "pci", provides "block".
 * Target: Intel 6/7-series EHCI (PCI class 0C/03/20, device 1C2D/1C26)
 * and any standard EHCI controller — the USB2 path used on platforms
 * without xHCI (e.g. H61/B75 with only EHCI controllers).
 *
 * Synchronous polling model (no IRQ): async list + QH/TD execution,
 * transfer completion detected by polling the QH overlay token.
 *
 * Pipeline:
 *   1. PCI discovery: class=0x0C/03/20
 *   2. MMIO mapping: BAR0 via mm_map_mmio / HHDM
 *   3. HC init: HCRESET, port power, RUN
 *   4. Port scan: reset, wait PED, read speed
 *   5. Enumeration: SET_ADDRESS, GET_DESCRIPTOR(dev), SET_CONFIGURATION
 *   6. MSC: BOT (CBW/CSW) + SCSI (INQUIRY/RD_CAP/READ10/WRITE10)
 *   7. Block provider: register usb2 via kernel_api.block
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(ehci_pci_found);
DKM_STAT_DECL(ehci_port_ok);
DKM_STAT_DECL(ehci_msc_found);
DKM_STAT_DECL(ehci_block_reads);
DKM_STAT_DECL(ehci_block_writes);

static const char *const g_depends[] = { "pci" };
static const char *const g_provides[] = { "block" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "ehci",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 6,   /* DKM_CLASS_STORAGE */
    .stage          = 1,
    .flags          = 0,
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

/* ================================================================
 *  Constants — EHCI registers, QH/TD, USB protocol
 * ================================================================ */

#define EHCI_CLASS_SERIAL  0x0C
#define EHCI_SUBCLASS_USB 0x03
#define EHCI_PROGIF_EHCI  0x20

/* Capability registers (MMIO + 0x00) */
#define EHCI_CAP_CAPLENGTH  0x00
#define EHCI_CAP_HCSPARAMS  0x04
#define EHCI_CAP_HCCPARAMS  0x08

/* Operational registers (MMIO + caplength) */
#define EHCI_OP_USBCMD      0x00
#define EHCI_OP_USBSTS      0x04
#define EHCI_OP_USBINTR     0x08
#define EHCI_OP_FRINDEX     0x0C
#define EHCI_OP_ASYNCLIST   0x18
#define EHCI_OP_CONFIGFLAG  0x40
#define EHCI_OP_PORTSC      0x44
#define EHCI_PORT_STRIDE    0x04

/* USBCMD bits */
#define USBCMD_RUN      (1u << 0)
#define USBCMD_HCRESET  (1u << 1)
#define USBCMD_PSE      (1u << 4)
#define USBCMD_ASE      (1u << 5)

/* USBSTS bits */
#define USBSTS_USBINT   (1u << 0)
#define USBSTS_PCD      (1u << 2)
#define USBSTS_FLR      (1u << 3)
#define USBSTS_HSE      (1u << 4)
#define USBSTS_IAA      (1u << 6)
#define USBSTS_HCHALT   (1u << 12)

/* PORTSC bits — 与 QEMU include/hw/usb/ehci-regs.h / EHCI 1.0 spec 一致
 * BUG-FIX: 原定义位错（PED=bit1 应为 bit2，speed 在 LINESTAT bit10:11 而非
 * bit14:15，change 位是 bit1/3/5 而非 bit17/18/20/21）——端口永不使能、
 * 速度永远读 0，导致 HC 不执行该端口上的传输。 */
#define PORTSC_CONNECT  (1u << 0)  /* CCS */
#define PORTSC_CSC      (1u << 1)  /* Connect Status Change (W1C) */
#define PORTSC_PED      (1u << 2)  /* Port Enable */
#define PORTSC_PEDC     (1u << 3)  /* Port Enable/Disable Change (W1C) */
#define PORTSC_OCA      (1u << 4)  /* Over-Current Active */
#define PORTSC_OCC      (1u << 5)  /* Over-Current Change (W1C) */
#define PORTSC_FPR      (1u << 6)  /* Force Port Resume */
#define PORTSC_SUSP     (1u << 7)  /* Suspend */
#define PORTSC_PR       (1u << 8)  /* Port Reset */
#define PORTSC_LS_SHIFT 10         /* Line Status = 端口速度 (bit10:11) */
#define PORTSC_LS_MASK  0x3
#define PORTSC_PP       (1u << 12) /* Port Power */
#define PORTSC_POWNER   (1u << 13)

/* RWC 位（W1C）：CSC|PEDC|OCC —— 清 change 时写这些位 */
#define PORTSC_RWC      (PORTSC_CSC | PORTSC_PEDC | PORTSC_OCC)

/* PLS/LS 值（Line Status 字段，非 PLS 状态机） */
#define LS_FULL    0
#define LS_LOW     1
#define LS_HIGH    2

/* TD token bits */
#define TD_ACTIVE   (1u << 7)
#define TD_HALTED   (1u << 6)
#define TD_BUFERR   (1u << 5)
#define TD_BABBLE   (1u << 4)
#define TD_XACTERR  (1u << 3)
#define TD_PID_SHIFT 8
#define TD_PID_MASK  0x3
#define TD_CERR_SHIFT 10
#define TD_CERR_MASK 0x3
#define TD_IOC       (1u << 15)
#define TD_BYTES_SHIFT 16
#define TD_BYTES_MASK 0x7FFF
#define TD_TOGGLE    (1u << 31)

#define PID_OUT    0
#define PID_IN     1
#define PID_SETUP  2

/* QH 链接：最低位 1 = 列表尾（T 位），0 = QH 指针 */
#define QH_TERMINATE 0x1u

/* USB speeds (EPCAP/QH ep speed 字段) */
#define USB_SPEED_FULL  0
#define USB_SPEED_LOW   1
#define USB_SPEED_HIGH  2

/* BOT signatures */
#define BOT_CBW_SIG  0x43425355u  /* "USBC" */
#define BOT_CSW_SIG  0x53425355u  /* "USBS" */

/* SCSI opcodes */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A

/* ================================================================
 *  Data structures
 * ================================================================ */

/* EHCI Transfer Descriptor — 32 字节，32 字节对齐 */
typedef struct {
    u32 next;      /* +0  */
    u32 alt_next;  /* +4  */
    u32 token;     /* +8  */
    u32 buf[5];    /* +12 */
} ehci_td;

/* EHCI Queue Head — 44 字节，32 字节对齐
 * 布局必须与 QEMU v11 EHCIqh / EHCI 1.0 spec 完全一致：
 *   +0  hlink(next), +4  epchar, +8  epcap(端点能力: hub addr+smask/cmask),
 *   +12 current_qtd, +16 next_qtd(overlay), +20 altnext(overlay), +24 token(overlay),
 *   +28 buf[5]
 * BUG-FIX(关键): epcap 字段不能删！QEMU 从 +16 读 next_qtd、+20 读 altnext、
 * +24 读 token。删掉 epcap 使 overlay 错位 4 字节——驱动写的 ov_next 落到
 * current_qtd 位、ov_alt(0x1=T) 落到 next_qtd 位 → QEMU 认为 QH 无 TD
 * 永不 FETCHQTD（token 恒 ACTIVE 0x80280）。 */
typedef struct {
    u32 hlink;     /* +0  */
    u32 epchar;    /* +4  */
    u32 epcap;     /* +8  */
    u32 curqtd;    /* +12 */
    u32 ov_next;   /* +16 overlay.next_qtd */
    u32 ov_alt;    /* +20 overlay.altnext */
    u32 ov_token;  /* +24 overlay.token */
    u32 ov_buf[5]; /* +28 */
} ehci_qh;

/* MSC device state */
typedef struct {
    u8  dev_addr;
    u8  speed;
    u8  bulk_out_ep;   /* endpoint number (1..15) */
    u8  bulk_in_ep;
    u16 bulk_out_mps;
    u16 bulk_in_mps;
    u32 cbw_tag;
    u64 sector_count;
    u32 sector_size;
    int ready;
} ehci_msc_dev;

/* ================================================================
 *  Global state
 * ================================================================ */

static const struct dkm_kernel_api *g_api;
static const struct dkm_log_api *g_log;
static volatile u8 *g_mmio;      /* EHCI BAR0 virtual */
static u32 g_caplen;
static u32 g_nports;
static u32 g_64bit;              /* HCCPARAMS bit0 */
static u32 g_ppc;                /* per-port power control bit4 */

/* DMA：异步列表区（QH/TD 池）+ 数据缓冲 + CSW 缓冲 */
static struct dkm_dma_buffer g_qh_dma;    /* 4KB：QH×4 + TD×8 */
static struct dkm_dma_buffer g_xfer;      /* 数据 bounce buffer（>=64KB） */
static struct dkm_dma_buffer g_csw;       /* CSW 独立缓冲（防覆盖数据区前 13 字节） */
static ehci_qh *g_qh_ctrl;
static ehci_qh *g_qh_bulk_out;
static ehci_qh *g_qh_bulk_in;
static ehci_td *g_td_pool;
static u32 g_td_next;

static ehci_msc_dev g_msc;
static u8 g_pci_bus, g_pci_dev, g_pci_func;

/* ================================================================
 *  Utility
 * ================================================================ */

static void log_hex(const char *prefix, u64 value) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    u32 pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--)
        buf[pos++] = hex[(value >> (i * 4)) & 0xf];
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

static void ehci_zero(void *ptr, u32 len) {
    u8 *p = (u8 *)ptr;
    for (u32 i = 0; i < len; i++) p[i] = 0;
}

static void ehci_memcpy(void *dst, const void *src, u32 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < len; i++) d[i] = s[i];
}

static u32 mmio_read32(u32 off) {
    return *(volatile u32 *)(void *)(g_mmio + off);
}

static void mmio_write32(u32 off, u32 value) {
    *(volatile u32 *)(void *)(g_mmio + off) = value;
}

static void ehci_mb(void) {
    __asm__ volatile("" ::: "memory");
}

/* 把物理地址编码进 32 位链接字段（64 位控制器用 CTRLDSSEGMENT 高 32 位） */
/* （内联使用，见 qh_submit_control/qh_enqueue） */

/* ================================================================
 *  Phase 1: PCI discovery + MMIO mapping
 * ================================================================ */

static int ehci_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vd = dkm_pci_read((u8)bus, dev, 0, PCI_VENDOR_ID);
            if ((vd & 0xffff) == 0xffff) continue;
            u8 header = (u8)(dkm_pci_read((u8)bus, dev, 0, PCI_HEADER) >> 16);
            u8 func_count = (header & 0x80) ? 8 : 1;
            for (u8 func = 0; func < func_count; func++) {
                u32 vd2 = dkm_pci_read((u8)bus, dev, func, PCI_VENDOR_ID);
                if ((vd2 & 0xffff) == 0xffff) continue;
                u32 class_reg = dkm_pci_read((u8)bus, dev, func, 0x08);
                u8 prog_if   = (u8)((class_reg >> 8)  & 0xff);
                u8 subclass  = (u8)((class_reg >> 16) & 0xff);
                u8 class_code = (u8)((class_reg >> 24) & 0xff);
                if (class_code == EHCI_CLASS_SERIAL &&
                    subclass  == EHCI_SUBCLASS_USB &&
                    prog_if   == EHCI_PROGIF_EHCI) {
                    *out_bus  = (u8)bus;
                    *out_dev  = dev;
                    *out_func = func;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static volatile u8 *ehci_map_bar(u64 bar_phys) {
    u64 bar_virt = g_api->hhdm_offset + bar_phys;
    if (g_api->mm_map_mmio) {
        void *mapped = g_api->mm_map_mmio(bar_phys, 0x1000);
        if (mapped) {
            g_log->info("[ehci] BAR mapped via mm_map_mmio");
            return (volatile u8 *)mapped;
        }
    }
    if (g_api->mmio && g_api->mmio->is_mapped) {
        if (g_api->mmio->is_mapped(bar_virt) > 0) {
            g_log->info("[ehci] HHDM covers BAR");
            return (volatile u8 *)(uintptr_t)bar_virt;
        }
        if (g_api->mmio->map_mmio) {
            if (g_api->mmio->map_mmio(bar_phys, 0x1000) == 0 &&
                g_api->mmio->is_mapped(bar_virt) > 0) {
                g_log->info("[ehci] BAR mapped via page tables");
                return (volatile u8 *)(uintptr_t)bar_virt;
            }
        }
    }
    if (bar_phys < 0x100000000ULL) {
        g_log->info("[ehci] assuming HHDM covers low BAR");
        return (volatile u8 *)(uintptr_t)bar_virt;
    }
    g_log->error("[ehci] BAR MMIO unavailable");
    return 0;
}

/* ================================================================
 *  Phase 1: HC reset and start
 * ================================================================ */

static int ehci_hc_reset(void) {
    /* 等待 HCHALT=1（Halted 时才能写 HCRESET） */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (dkm_rdtsc() < deadline) {
        if (mmio_read32(g_caplen + EHCI_OP_USBSTS) & USBSTS_HCHALT) break;
        __asm__ volatile("pause");
    }

    mmio_write32(g_caplen + EHCI_OP_USBCMD, USBCMD_HCRESET);
    deadline = dkm_rdtsc() + dkm_tsc_per_ms * 100;
    while (dkm_rdtsc() < deadline) {
        if (!(mmio_read32(g_caplen + EHCI_OP_USBCMD) & USBCMD_HCRESET)) {
            g_log->info("[ehci] HC reset complete");
            return 0;
        }
        __asm__ volatile("pause");
    }
    g_log->error("[ehci] HC reset timeout");
    return -1;
}

/* ================================================================
 *  Phase 2: QH/TD management
 * ================================================================ */

static ehci_td *td_alloc(void) {
    if (g_td_next >= 8) return 0;
    ehci_td *td = &g_td_pool[g_td_next++];
    ehci_zero(td, sizeof(ehci_td));
    td->next = QH_TERMINATE;
    td->alt_next = QH_TERMINATE;
    return td;
}

/* 初始化一个 QH：endpoint=0 控制 / 指定端点 bulk。
 * 注意：不能动 hlink —— ehci_hc_start 在启动时已把 ctrl/out/in 三个 QH
 * 链成环；每次提交只更新 epchar/overlay，hlink 必须保留（否则环断裂，
 * HC 停止遍历，overlay token 恒 0 → qh_wait 假成功返回空数据）。 */
static void qh_init(ehci_qh *qh, u8 dev_addr, u8 ep_num, u8 speed,
                    u16 mps, int control) {
    qh->curqtd = 0;
    qh->ov_next = QH_TERMINATE;
    qh->ov_alt = QH_TERMINATE;
    qh->ov_token = 0;  /* inactive */

    u32 epchar = (u32)dev_addr;                 /* [6:0] */
    epchar |= ((u32)ep_num << 8);               /* [11:8] */
    epchar |= ((u32)speed << 12);               /* [14:12] */
    epchar |= ((u32)mps << 16);                 /* [26:16] */
    if (control) epchar |= (1u << 27);          /* control endpoint */
    /* DTC 清 0：由 HC 管理 data toggle。
     * H 位(bit15) 保留：ctrl 是环头 QH，每次提交重建 epchar 后必须
     * 重新置 H，否则 QEMU WAITLISTHEAD 找不到入口（只在 H=1 的 QH
     * 处进入 FETCHENTRY）。 */
    u32 keep_h = qh->epchar & (1u << 15);
    qh->epchar = epchar | keep_h;
    /* epcap（端点能力）：直连根端口 hub addr=0, smask/cmask=0。
     * 该字段必须存在且清零——QEMU 依赖它使 overlay 偏移正确。 */
    qh->epcap = 0;
}

/* 构建控制传输 TD 链：SETUP -> [DATA] -> STATUS，挂到 qh。 */
static int qh_submit_control(ehci_qh *qh, const u8 *setup8,
                             u8 *data, u32 data_len, u8 dir) {
    /* 清空旧链：重置 overlay */
    qh->curqtd = 0;
    qh->ov_next = QH_TERMINATE;
    qh->ov_token = 0;
    g_td_next = 0;  /* 复用池（单传输假设） */

    ehci_td *t_setup = td_alloc();
    ehci_td *t_data = 0;
    ehci_td *t_status = td_alloc();
    if (!t_setup || !t_status) return -1;
    if (data_len > 0) {
        t_data = td_alloc();
        if (!t_data) return -2;
    }

    /* SETUP TD：8 字节，toggle=0，pid=SETUP
     * 注意：必须先分配 t_data 再算 t_setup->next（原实现引用未分配
     * 的 t_data → TD 链指向垃圾地址 → HC 走链崩溃/超时）。 */
    if (data_len > 0) {
        t_setup->next = (u32)(g_qh_dma.phys + (u64)((u8 *)t_data - (u8 *)g_qh_dma.virt));
    } else {
        t_setup->next = (u32)(g_qh_dma.phys + (u64)((u8 *)t_status - (u8 *)g_qh_dma.virt));
    }
    /* setup 数据嵌入 QH overlay buffer？标准做法：SETUP TD buffer 指向含 8 字节 setup 的内存 */
    /* 简化：setup 包拷贝到 xfer 缓冲头部，TD buffer 指向它 */
    {
        u8 *sb = (u8 *)g_xfer.virt;
        ehci_memcpy(sb, setup8, 8);
        __asm__ volatile("wbinvd" ::: "memory");
        t_setup->buf[0] = (u32)(g_xfer.phys & 0xFFFFFFFFu);
        u32 tok = TD_ACTIVE | (PID_SETUP << TD_PID_SHIFT) | (8u << TD_BYTES_SHIFT);
        t_setup->token = tok;  /* toggle=0 */
    }

    /* DATA TD（可选）——t_data 已在上方分配，此处不再重复分配 */
    if (data_len > 0 && t_data) {
        /* 数据在 g_xfer.virt + 8 偏移处（IN：设备写这里；OUT：软件先拷入） */
        u8 *db = (u8 *)g_xfer.virt + 8;
        if (dir == PID_OUT) {
            ehci_memcpy(db, data, data_len);
            __asm__ volatile("wbinvd" ::: "memory");
        }
        t_data->buf[0] = (u32)((g_xfer.phys + 8) & 0xFFFFFFFFu);
        t_data->next = (u32)(g_qh_dma.phys + (u64)((u8 *)t_status - (u8 *)g_qh_dma.virt));
        u32 tok = TD_ACTIVE | ((u32)dir << TD_PID_SHIFT) | ((u32)data_len << TD_BYTES_SHIFT) | TD_TOGGLE;
        t_data->token = tok;  /* toggle=1 */
    }

    /* STATUS TD：反向 pid，toggle=1，长度 0 */
    {
        u8 spid = (dir == PID_IN) ? PID_OUT : PID_IN;
        if (data_len == 0) spid = (dir == PID_IN) ? PID_OUT : PID_IN;
        u32 tok = TD_ACTIVE | ((u32)spid << TD_PID_SHIFT) | TD_TOGGLE;
        t_status->token = tok;
        t_status->next = QH_TERMINATE;
    }

    /* QH overlay 指向第一个 TD */
    qh->ov_next = (u32)(g_qh_dma.phys + (u64)((u8 *)t_setup - (u8 *)g_qh_dma.virt));
    /* 写完 QH/TD 后刷新 CPU cache——DMA 控制器读物理内存需要；
     * 缺此步在 WHPX 下 QEMU 读不到 ov_next（竞态），TCG 下侥幸通过。 */
    __asm__ volatile("wbinvd" ::: "memory");
    ehci_mb();
    return 0;
}

/* 构建 bulk 传输单 TD：dir 0=OUT 1=IN。
 * in_dma: IN 方向的目标 DMA 缓冲（数据用 g_xfer，CSW 用 g_csw——
 * 防止 CSW 读覆盖数据区前 13 字节）。OUT 方向 in_dma 忽略（数据先拷入
 * in_dma 指向的缓冲）。 */
static int qh_submit_bulk(ehci_qh *qh, u8 dir, const void *data, u32 data_len,
                          struct dkm_dma_buffer *in_dma) {
    qh->curqtd = 0;
    qh->ov_next = QH_TERMINATE;
    qh->ov_token = 0;
    g_td_next = 0;

    ehci_td *td = td_alloc();
    if (!td) return -1;

    struct dkm_dma_buffer *dst = in_dma ? in_dma : &g_xfer;
    u8 *buf = (u8 *)dst->virt;
    if (dir == PID_OUT) {
        ehci_memcpy(buf, data, data_len);
        __asm__ volatile("wbinvd" ::: "memory");
    }
    td->buf[0] = (u32)(dst->phys & 0xFFFFFFFFu);
    u32 tok = TD_ACTIVE | ((u32)dir << TD_PID_SHIFT) | ((u32)data_len << TD_BYTES_SHIFT);
    td->token = tok;  /* toggle 由 HC 管理（DTC=0） */

    qh->ov_next = (u32)(g_qh_dma.phys + (u64)((u8 *)td - (u8 *)g_qh_dma.virt));
    __asm__ volatile("wbinvd" ::: "memory");
    ehci_mb();
    return 0;
}

/* 等待 QH 上的传输完成。轮询首个 TD 的 token（HC 执行后清 ACTIVE、
 * 出错置 HALTED）——比检查 overlay 可靠：HC 未执行时 overlay token
 * 保持 inactive，轮询 overlay 会假成功返回空数据。first_td 传 TD 池指针，
 * 由调用方从 qh->ov_next 反推或直接记录。
 * BUG-FIX（缓存一致性）：EHCI HC 经 DMA 写 TD 到物理内存，CPU 轮询前
 * 必须失效缓存行，否则读到 write-back cache 旧值（Active 恒 1）→ 假超时。
 * QEMU 复现证据：first_td token=0x80280 显示 SETUP 已执行成功（Active 清、
 * 无错误位），但轮询超时——正是 cache 陈旧。 */
static int qh_wait(ehci_qh *qh, ehci_td *first_td, u32 timeout_ms) {
    (void)qh;
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * timeout_ms;
    for (;;) {
        /* 失效 TD 所在缓存行，强制从内存读 HC 的 DMA 写结果 */
        __asm__ volatile("clflush (%0)" : : "r"(first_td) : "memory");
        u32 tok = first_td->token;
        if (!(tok & TD_ACTIVE)) {
            if (tok & TD_HALTED) {
                g_log->warn("[ehci] transfer halted");
                log_hex("[ehci] token=", tok);
                return -2;
            }
            return 0;
        }
        if (dkm_rdtsc() > deadline) {
            g_log->warn("[ehci] transfer timeout");
            log_hex("[ehci] first_td token=", tok);
            return -1;
        }
        __asm__ volatile("pause");
    }
}

/* 启动 HC：把三个 QH 链成环（ctrl→bulk_out→bulk_in→ctrl），
 * 在 ASE=0（async schedule 停止）时写一次 ASYNCLIST，然后 RUN|ASE。
 * 之后每次提交只改 QH overlay，不再碰 ASYNCLIST（规范要求该寄存器
 * 只能在 async schedule 停止时写——QEMU 会拒绝运行期写入）。 */
static int ehci_hc_start(void) {
    u64 ctrl_phys  = g_qh_dma.phys + (u64)((u8 *)g_qh_ctrl - (u8 *)g_qh_dma.virt);
    u64 out_phys   = g_qh_dma.phys + (u64)((u8 *)g_qh_bulk_out - (u8 *)g_qh_dma.virt);
    u64 in_phys    = g_qh_dma.phys + (u64)((u8 *)g_qh_bulk_in - (u8 *)g_qh_dma.virt);

    /* hlink 链接指针必须带类型位 bit1=1（QH 类型）。
     * QEMU v11 的 NLPTR_TYPE_GET(x)=((x)>>1)&3，QH 类型=1 → 写物理地址|2。
     * BUG-FIX: 裸地址（bit1=0）→ QEMU fetchentry 报 "non queue head
     * request in async schedule" 并重置 HC，TD 永不执行。 */
    g_qh_ctrl->hlink     = (u32)((out_phys & 0xFFFFFFFFu) | 2u);
    g_qh_bulk_out->hlink = (u32)((in_phys & 0xFFFFFFFFu) | 2u);
    g_qh_bulk_in->hlink  = (u32)((ctrl_phys & 0xFFFFFFFFu) | 2u);
    /* H 位(bit15) = 回收头标记：QEMU 的 WAITLISTHEAD 状态只在环中找到
     * H=1 的 QH 时进入 FETCHENTRY→FETCHQH。不置 H 位 → 状态机永远停在
     * WAITLISTHEAD，TD 永不执行。ctrl 作为环头必须置 H。 */
    g_qh_ctrl->epchar |= (1u << 15);
    ehci_mb();

    /* 确保 ASE=0（复位后默认 0） */
    u32 cmd = mmio_read32(g_caplen + EHCI_OP_USBCMD);
    mmio_write32(g_caplen + EHCI_OP_USBCMD, cmd & ~USBCMD_ASE);
    ehci_mb();
    mmio_write32(g_caplen + EHCI_OP_ASYNCLIST, (u32)((ctrl_phys & 0xFFFFFFFFu) | 2u));
    ehci_mb();
    mmio_write32(g_caplen + EHCI_OP_USBCMD,
                 mmio_read32(g_caplen + EHCI_OP_USBCMD) | USBCMD_ASE | USBCMD_RUN);
    ehci_mb();
    g_log->info("[ehci] async schedule started");
    return 0;
}

/* 简单控制传输封装 */
static int ehci_control(u8 dev_addr, u8 speed, u16 mps0,
                        const u8 *setup8, u8 *data, u32 data_len, u8 dir) {
    qh_init(g_qh_ctrl, dev_addr, 0, speed, mps0, 1);
    if (qh_submit_control(g_qh_ctrl, setup8, data, data_len, dir) != 0) return -1;
    int rc = qh_wait(g_qh_ctrl, &g_td_pool[0], 5000);
    /* IN 方向：失效数据缓冲缓存行后把 HC DMA 写入的数据拷回调用方 */
    if (rc == 0 && dir == PID_IN && data && data_len > 0) {
        __asm__ volatile("clflush (%0)" : : "r"(g_xfer.virt) : "memory");
        __asm__ volatile("clflush 64(%0)" : : "r"(g_xfer.virt) : "memory");
        ehci_memcpy(data, (u8 *)g_xfer.virt + 8, data_len);
    }
    return rc;
}

/* 简单 bulk 传输封装（端点号已含方向编码在 QH）。
 * in_dma: IN 方向的目标 DMA 缓冲（g_xfer 或 g_csw）。
 * BUG-FIX: CSW 必须用独立缓冲——若与数据共用 g_xfer，13 字节 CSW 会
 * 覆盖数据区前 13 字节（bps 字段被清零）。 */
static int ehci_bulk(ehci_qh *qh, u8 dev_addr, u8 speed, u16 mps,
                     u8 ep_num, u8 dir, const void *data, u32 data_len,
                     struct dkm_dma_buffer *in_dma) {
    qh_init(qh, dev_addr, ep_num, speed, mps, 0);
    if (qh_submit_bulk(qh, dir, data, data_len, in_dma) != 0) return -1;
    int rc = qh_wait(qh, &g_td_pool[0], 10000);
    if (rc == 0 && dir == PID_IN) {
        struct dkm_dma_buffer *dst = in_dma ? in_dma : &g_xfer;
        /* 失效缓存行后拷贝 */
        __asm__ volatile("clflush (%0)" : : "r"(dst->virt) : "memory");
        ehci_memcpy((void *)data, dst->virt, data_len);
    }
    return rc;
}

/* ================================================================
 *  Phase 3: Port scan and reset
 * ================================================================ */

static int ehci_reset_port(u32 port) {
    u32 po = g_caplen + EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    u32 ps = mmio_read32(po);
    if (!(ps & PORTSC_CONNECT)) return -1;

    /* 复位序列（EHCI 1.0 §4.2.4 + QEMU 模型兼容）：
     * 1) 置 PR=1 请求复位
     * 2) 保持 PR=1 一小段时间（QEMU 不置 PRC，等待只是给模拟器处理窗口）
     * 3) 清 PR —— QEMU 在 PR 1→0 下降沿执行 usb_port_reset 并置 PED（仅高速）
     * 4) 轮询 PED 确认使能
     * BUG-FIX: 原实现等待 PRC(bit20)，但 QEMU 从不置该位 → 200ms 必超时
     * 枚举中止（QEMU hcd-ehci.c ehci_port_write 只处理 PR 下降沿）。 */
    mmio_write32(po, ps | PORTSC_PR);
    dkm_delay_ms(50);

    /* 清 PR + 清 change 位（1→0 下降沿触发复位） */
    ps = mmio_read32(po);
    mmio_write32(po, (ps & ~PORTSC_PR) | PORTSC_RWC);

    /* 轮询 PED */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 200;
    while (dkm_rdtsc() < deadline) {
        ps = mmio_read32(po);
        if (ps & PORTSC_PED) {
            DKM_STAT_INC(ehci_port_ok);
            return 0;
        }
        if (!(ps & PORTSC_CONNECT)) return -2;  /* 复位中断开 */
        __asm__ volatile("pause");
    }
    g_log->warn("[ehci] port not enabled after reset");
    return -3;
}

static u8 ehci_port_speed(u32 port) {
    u32 po = g_caplen + EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    u32 ps = mmio_read32(po);
    return (u8)((ps >> PORTSC_LS_SHIFT) & PORTSC_LS_MASK);
}

/* ================================================================
 *  Phase 4: USB enumeration
 * ================================================================ */

/* 前置声明（BOT/SCSI 定义在 enum_msc 之后） */
static int ehci_scsi_inquiry(void);
static int ehci_bot_send_cbw(u8 lun, u8 flags, u32 data_len,
                             const u8 *cb, u8 cb_len);
static int ehci_bot_receive_csw(void);

static int ehci_get_device_descriptor(u8 *out, u32 len) {
    u8 setup[8];
    setup[0] = 0x80; setup[1] = 0x06;
    setup[2] = 0x00; setup[3] = 0x01;  /* Device descriptor */
    setup[4] = 0x00; setup[5] = 0x00;
    setup[6] = (u8)(len & 0xFF); setup[7] = (u8)((len >> 8) & 0xFF);
    ehci_zero(out, len);
    /* GET_DESCRIPTOR 阶段 EP0 MPS 未知，按速度用默认值 */
    u16 mps0 = 64;
    if (g_msc.speed == LS_LOW) mps0 = 8;
    return ehci_control(g_msc.dev_addr, g_msc.speed, mps0, setup, out, len, PID_IN);
}

static int ehci_set_address(u8 addr, u16 mps0) {
    u8 setup[8];
    setup[0] = 0x00; setup[1] = 0x05;
    setup[2] = (u8)(addr & 0xFF); setup[3] = 0x00;
    setup[4] = 0x00; setup[5] = 0x00;
    setup[6] = 0x00; setup[7] = 0x00;
    return ehci_control(0, g_msc.speed, mps0, setup, 0, 0, PID_OUT);
}

static int ehci_set_configuration(u8 addr, u8 cfg, u16 mps0) {
    u8 setup[8];
    setup[0] = 0x00; setup[1] = 0x09;
    setup[2] = cfg; setup[3] = 0x00;
    setup[4] = 0x00; setup[5] = 0x00;
    setup[6] = 0x00; setup[7] = 0x00;
    return ehci_control(addr, g_msc.speed, mps0, setup, 0, 0, PID_OUT);
}

static int ehci_get_config_descriptor(u8 addr, u8 *out, u32 len) {
    u8 setup[8];
    setup[0] = 0x80; setup[1] = 0x06;
    setup[2] = 0x00; setup[3] = 0x02;  /* Configuration descriptor */
    setup[4] = 0x00; setup[5] = 0x00;
    setup[6] = (u8)(len & 0xFF); setup[7] = (u8)((len >> 8) & 0xFF);
    ehci_zero(out, len);
    return ehci_control(addr, g_msc.speed, 64, setup, out, len, PID_IN);
}

/* 枚举 MSC 设备：SET_ADDRESS -> GET_DESCRIPTOR -> SET_CONFIGURATION -> 端点解析 */
static int ehci_enum_msc(u32 port) {
    g_msc.speed = ehci_port_speed(port);
    log_hex("[ehci] port speed=", g_msc.speed);
    u16 mps0 = 64;  /* 全速/高速 EP0 默认 64 */
    if (g_msc.speed == LS_LOW) mps0 = 8;

    /* SET_ADDRESS（用默认 pipe，mps 按速度） */
    if (ehci_set_address(1, mps0) != 0) {
        g_log->warn("[ehci] SET_ADDRESS failed");
        return -1;
    }
    g_msc.dev_addr = 1;
    g_msc.bulk_out_mps = mps0;  /* 后续控制传输用已知 mps0 */

    /* GET_DESCRIPTOR(device, 18) */
    u8 dev_desc[18];
    if (ehci_get_device_descriptor(dev_desc, 18) != 0) {
        g_log->warn("[ehci] GET_DESCRIPTOR(Device) failed");
        return -2;
    }
    u8 dev_class = dev_desc[4];
    u16 vid = (u16)dev_desc[8] | ((u16)dev_desc[9] << 8);
    u16 pid = (u16)dev_desc[10] | ((u16)dev_desc[11] << 8);
    u8 real_mps0 = dev_desc[7];
    log_hex("[ehci] dev class=", dev_class);
    log_hex("[ehci] VID=", vid);
    log_hex("[ehci] PID=", pid);
    log_hex("[ehci] bMaxPacketSize0=", real_mps0);
    /* 设备声明的真实 EP0 MPS 与猜测不同时，用真实值 */
    if (real_mps0 >= 8 && real_mps0 <= 64) mps0 = real_mps0;

    /* SET_CONFIGURATION(1) */
    if (ehci_set_configuration(g_msc.dev_addr, 1, mps0) != 0) {
        g_log->warn("[ehci] SET_CONFIGURATION failed");
        return -3;
    }

    /* GET_DESCRIPTOR(config, 255) 并解析 MSC 接口 + bulk 端点 */
    u8 cfg_desc[255];
    if (ehci_get_config_descriptor(g_msc.dev_addr, cfg_desc, 255) != 0) {
        g_log->warn("[ehci] GET_DESCRIPTOR(Config) failed");
        return -4;
    }
    u16 total_len = (u16)cfg_desc[2] | ((u16)cfg_desc[3] << 8);
    u8 bulk_out_ep = 0, bulk_in_ep = 0;
    u16 bulk_out_mps = 512, bulk_in_mps = 512;
    int msc_found = 0;

    u32 pos = 9;  /* 跳过 config descriptor */
    while (pos + 2 <= (u32)total_len && pos + cfg_desc[pos] <= (u32)total_len) {
        u8 desc_len = cfg_desc[pos];
        u8 desc_type = cfg_desc[pos + 1];
        if (desc_type == 4 && desc_len >= 9) {
            u8 iface_class = cfg_desc[pos + 5];
            u8 iface_sub = cfg_desc[pos + 6];
            u8 iface_proto = cfg_desc[pos + 7];
            if (iface_class == 0x08 && iface_sub == 0x06 && iface_proto == 0x50) {
                msc_found = 1;
                g_log->info("[ehci] MSC BOT interface found");
                u32 ep_pos = pos + desc_len;
                while (ep_pos + 2 <= (u32)total_len) {
                    u8 ep_len = cfg_desc[ep_pos];
                    if (ep_len == 0 || ep_pos + ep_len > (u32)total_len) break;
                    if (cfg_desc[ep_pos + 1] == 5) {
                        u8 ep_addr = cfg_desc[ep_pos + 2];
                        u8 ep_attr = cfg_desc[ep_pos + 3];
                        u16 ep_mps = (u16)cfg_desc[ep_pos + 4] | ((u16)cfg_desc[ep_pos + 5] << 8);
                        if ((ep_attr & 0x03) == 0x02) {
                            if (ep_addr & 0x80) {
                                bulk_in_ep = ep_addr & 0x0F;
                                bulk_in_mps = ep_mps;
                            } else {
                                bulk_out_ep = ep_addr & 0x0F;
                                bulk_out_mps = ep_mps;
                            }
                        }
                    } else if (cfg_desc[ep_pos + 1] == 4) {
                        break;
                    }
                    ep_pos += ep_len;
                }
                break;
            }
        }
        pos += desc_len;
    }

    if (!msc_found || bulk_out_ep == 0 || bulk_in_ep == 0) {
        g_log->warn("[ehci] no MSC BOT interface found");
        return -5;
    }
    log_hex("[ehci] Bulk OUT EP=", bulk_out_ep);
    log_hex("[ehci] Bulk IN  EP=", bulk_in_ep);
    log_hex("[ehci] Bulk OUT MPS=", bulk_out_mps);
    log_hex("[ehci] Bulk IN  MPS=", bulk_in_mps);

    g_msc.bulk_out_ep = bulk_out_ep;
    g_msc.bulk_in_ep = bulk_in_ep;
    g_msc.bulk_out_mps = bulk_out_mps;
    g_msc.bulk_in_mps = bulk_in_mps;
    g_msc.cbw_tag = 1;

    /* INQUIRY 验证设备是块设备 */
    if (ehci_scsi_inquiry() != 0) {
        g_log->warn("[ehci] INQUIRY failed or not block device");
    }
    return 0;
}

/* ================================================================
 *  Phase 5: BOT + SCSI（复用 xhci 已验证的 CBW/CSW/SCSI 组包逻辑）
 * ================================================================ */

static int ehci_bot_send_cbw(u8 lun, u8 flags, u32 data_len,
                             const u8 *cb, u8 cb_len) {
    u8 cbw[31];
    ehci_zero(cbw, 31);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    u32 tag = g_msc.cbw_tag++;
    cbw[4] = (u8)(tag & 0xFF);
    cbw[5] = (u8)((tag >> 8) & 0xFF);
    cbw[6] = (u8)((tag >> 16) & 0xFF);
    cbw[7] = (u8)((tag >> 24) & 0xFF);
    cbw[8] = (u8)(data_len & 0xFF);
    cbw[9] = (u8)((data_len >> 8) & 0xFF);
    cbw[10] = (u8)((data_len >> 16) & 0xFF);
    cbw[11] = (u8)((data_len >> 24) & 0xFF);
    cbw[12] = flags;
    cbw[13] = lun;
    cbw[14] = cb_len;
    for (u8 i = 0; i < cb_len && i < 16; i++) cbw[15 + i] = cb[i];

    return ehci_bulk(g_qh_bulk_out, g_msc.dev_addr, g_msc.speed,
                     g_msc.bulk_out_mps, g_msc.bulk_out_ep, PID_OUT, cbw, 31, 0);
}

static int ehci_bot_receive_csw(void) {
    u8 csw[13];
    /* CSW 用独立 DMA 缓冲，避免覆盖数据阶段读入 g_xfer 的数据 */
    int rc = ehci_bulk(g_qh_bulk_in, g_msc.dev_addr, g_msc.speed,
                       g_msc.bulk_in_mps, g_msc.bulk_in_ep, PID_IN, csw, 13, &g_csw);
    if (rc != 0) return rc;
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53) {
        g_log->error("[ehci] CSW signature mismatch");
        return -2;
    }
    if (csw[12] != 0) {
        log_hex("[ehci] CSW status=", csw[12]);
        return -3;
    }
    return 0;
}

static int ehci_scsi_inquiry(void) {
    u8 cb[16];
    ehci_zero(cb, 16);
    cb[0] = SCSI_INQUIRY;
    cb[4] = 36;
    if (ehci_bot_send_cbw(0, 0x80, 36, cb, 6) != 0) return -1;
    u8 inq[36];
    if (ehci_bulk(g_qh_bulk_in, g_msc.dev_addr, g_msc.speed,
                  g_msc.bulk_in_mps, g_msc.bulk_in_ep, PID_IN, inq, 36, &g_xfer) != 0) return -2;
    if (ehci_bot_receive_csw() != 0) return -3;
    log_hex("[ehci] INQ PeripheralType=", inq[0] & 0x1F);
    return ((inq[0] & 0x1F) == 0) ? 0 : -4;
}

static int ehci_scsi_read_capacity(u64 *out_sectors, u32 *out_blk) {
    u8 cb[16];
    ehci_zero(cb, 16);
    cb[0] = SCSI_READ_CAPACITY10;
    if (ehci_bot_send_cbw(0, 0x80, 8, cb, 10) != 0) return -1;
    u8 cap[8];
    if (ehci_bulk(g_qh_bulk_in, g_msc.dev_addr, g_msc.speed,
                  g_msc.bulk_in_mps, g_msc.bulk_in_ep, PID_IN, cap, 8, &g_xfer) != 0) return -2;
    if (ehci_bot_receive_csw() != 0) return -3;
    u32 max_lba = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | (u32)cap[3];
    u32 blk = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) | ((u32)cap[6] << 8) | (u32)cap[7];
    *out_sectors = (u64)max_lba + 1;
    *out_blk = blk;
    log_hex("[ehci] capacity sectors=", *out_sectors);
    log_hex("[ehci] block size=", blk);
    return 0;
}

static int ehci_scsi_test_unit_ready(void) {
    u8 cb[16];
    ehci_zero(cb, 16);
    cb[0] = SCSI_TEST_UNIT_READY;
    if (ehci_bot_send_cbw(0, 0x00, 0, cb, 6) != 0) return -1;
    return ehci_bot_receive_csw();
}

static int ehci_scsi_read10(u64 lba, u32 count, void *buffer) {
    u8 cb[16];
    ehci_zero(cb, 16);
    cb[0] = SCSI_READ10;
    cb[2] = (u8)((lba >> 24) & 0xFF);
    cb[3] = (u8)((lba >> 16) & 0xFF);
    cb[4] = (u8)((lba >> 8) & 0xFF);
    cb[5] = (u8)(lba & 0xFF);
    cb[7] = (u8)((count >> 8) & 0xFF);
    cb[8] = (u8)(count & 0xFF);
    u32 data_len = count * 512;
    if (ehci_bot_send_cbw(0, 0x80, data_len, cb, 10) != 0) return -1;
    if (ehci_bulk(g_qh_bulk_in, g_msc.dev_addr, g_msc.speed,
                  g_msc.bulk_in_mps, g_msc.bulk_in_ep, PID_IN, buffer, data_len, &g_xfer) != 0) return -2;
    return ehci_bot_receive_csw();
}

static int ehci_scsi_write10(u64 lba, u32 count, const void *buffer) {
    u8 cb[16];
    ehci_zero(cb, 16);
    cb[0] = SCSI_WRITE10;
    cb[2] = (u8)((lba >> 24) & 0xFF);
    cb[3] = (u8)((lba >> 16) & 0xFF);
    cb[4] = (u8)((lba >> 8) & 0xFF);
    cb[5] = (u8)(lba & 0xFF);
    cb[7] = (u8)((count >> 8) & 0xFF);
    cb[8] = (u8)(count & 0xFF);
    u32 data_len = count * 512;
    if (ehci_bot_send_cbw(0, 0x00, data_len, cb, 10) != 0) return -1;
    if (ehci_bulk(g_qh_bulk_out, g_msc.dev_addr, g_msc.speed,
                  g_msc.bulk_out_mps, g_msc.bulk_out_ep, PID_OUT, buffer, data_len, 0) != 0) return -2;
    return ehci_bot_receive_csw();
}

/* ================================================================
 *  Block provider
 * ================================================================ */

static int ehci_block_read(void *ctx, u64 lba, u32 count, void *buffer) {
    (void)ctx;
    if (!g_msc.ready || !buffer || count == 0) return -1;
    DKM_STAT_INC(ehci_block_reads);
    u8 *buf = (u8 *)buffer;
    while (count > 0) {
        u32 chunk = count > 16 ? 16 : count;
        if (ehci_scsi_read10(lba, chunk, g_xfer.virt) != 0) {
            log_hex("[ehci] READ10 failed lba=", lba);
            return -1;
        }
        ehci_memcpy(buf, g_xfer.virt, chunk * 512);
        buf += chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ehci_block_write(void *ctx, u64 lba, u32 count, const void *buffer) {
    (void)ctx;
    if (!g_msc.ready || !buffer || count == 0) return -1;
    DKM_STAT_INC(ehci_block_writes);
    const u8 *buf = (const u8 *)buffer;
    while (count > 0) {
        u32 chunk = count > 16 ? 16 : count;
        ehci_memcpy(g_xfer.virt, buf, chunk * 512);
        __asm__ volatile("wbinvd" ::: "memory");
        if (ehci_scsi_write10(lba, chunk, g_xfer.virt) != 0) {
            log_hex("[ehci] WRITE10 failed lba=", lba);
            return -1;
        }
        buf += chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/* ================================================================
 *  driver_init
 * ================================================================ */

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    dkm_tsc_calibrate();
    if (!api || !api->log) return -1;
    g_api = api;
    g_log = api->log;
    dkm_instr_init(api);

    g_log->info("[ehci] init begin");

    /* PCI 发现 0C/03/20 */
    u8 bus = 0, dev = 0, func = 0;
    if (ehci_find(&bus, &dev, &func) != 0) {
        g_log->info("[ehci] EHCI controller not found");
        g_log->info("[ehci] driver ready");
        return 0;
    }
    g_pci_bus = bus; g_pci_dev = dev; g_pci_func = func;
    DKM_STAT_INC(ehci_pci_found);
    g_log->info("[ehci] EHCI controller found");
    log_hex("[ehci] bus=", bus);
    log_hex("[ehci] dev=", dev);
    log_hex("[ehci] func=", func);

    u32 vd = dkm_pci_read(bus, dev, func, PCI_VENDOR_ID);
    log_hex("[ehci] vendor=", vd & 0xffff);
    log_hex("[ehci] device=", vd >> 16);

    /* 使能 MEM + BUSM */
    u32 command = dkm_pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_MEM | PCI_CMD_BUSM;
    dkm_pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[ehci] PCI command=", command);

    /* BAR0（EHCI 通常是 32 位 MMIO） */
    u32 bar0_lo = dkm_pci_read(bus, dev, func, PCI_BAR0);
    u64 bar_phys;
    if ((bar0_lo & 0x7) == 0x4) {
        u32 bar1_hi = dkm_pci_read(bus, dev, func, PCI_BAR1);
        bar_phys = ((u64)bar1_hi << 32) | (u64)(bar0_lo & 0xFFFFFFF0u);
    } else {
        bar_phys = (u64)(bar0_lo & 0xFFFFFFF0u);
    }
    log_hex("[ehci] BAR0 phys=", bar_phys);
    if (!api->hhdm_offset || !bar_phys) {
        g_log->warn("[ehci] missing HHDM or BAR0");
        g_log->info("[ehci] driver ready");
        return 0;
    }

    g_mmio = ehci_map_bar(bar_phys);
    if (!g_mmio) {
        g_log->error("[ehci] BAR MMIO unavailable");
        g_log->info("[ehci] driver ready");
        return 0;
    }

    /* 能力寄存器 */
    g_caplen = mmio_read32(EHCI_CAP_CAPLENGTH) & 0xFF;
    u32 hcsp = mmio_read32(EHCI_CAP_HCSPARAMS);
    u32 hccp = mmio_read32(EHCI_CAP_HCCPARAMS);
    g_nports = hcsp & 0x0F;
    g_64bit = hccp & 1u;
    g_ppc = (hcsp >> 4) & 1u;
    log_hex("[ehci] CAPLEN=", g_caplen);
    log_hex("[ehci] nports=", g_nports);
    log_hex("[ehci] 64bit=", g_64bit);
    log_hex("[ehci] PPC=", g_ppc);
    if (g_nports == 0) {
        g_log->error("[ehci] no ports");
        g_log->info("[ehci] driver ready");
        return 0;
    }

    /* HC 复位 */
    if (ehci_hc_reset() != 0) {
        g_log->error("[ehci] HC reset failed");
        g_log->info("[ehci] driver ready");
        return 0;
    }

    /* 64 位控制器：CTRLDSSEGMENT = 0 */
    if (g_64bit) mmio_write32(g_caplen + 0x10, 0);

    /* DMA：异步列表页 + 数据缓冲 */
    if (!api->dma) {
        g_log->warn("[ehci] DMA API unavailable");
        g_log->info("[ehci] driver ready");
        return 0;
    }
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_qh_dma) != 0) {
        g_log->error("[ehci] async list alloc failed");
        g_log->info("[ehci] driver ready");
        return 0;
    }
    if (api->dma->alloc_pages(16, 4096, 0x100000000ULL, &g_xfer) != 0) {
        g_log->error("[ehci] xfer buf alloc failed");
        g_log->info("[ehci] driver ready");
        return 0;
    }
    if (api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_csw) != 0) {
        g_log->error("[ehci] csw buf alloc failed");
        g_log->info("[ehci] driver ready");
        return 0;
    }
    ehci_zero(g_qh_dma.virt, g_qh_dma.size);
    ehci_zero(g_xfer.virt, g_xfer.size);
    ehci_zero(g_csw.virt, g_csw.size);

    /* QH 布局：必须 32 字节对齐（EHCI 规范）。ctrl@0, out@64, in@128；
     * TD 池 @256（TD 32 字节对齐）。原 48 字节间距导致 out/in 未对齐，
     * HC 无法处理 QH → TD 永不被执行 → 超时。 */
    g_qh_ctrl = (ehci_qh *)((u8 *)g_qh_dma.virt + 0);
    g_qh_bulk_out = (ehci_qh *)((u8 *)g_qh_dma.virt + 64);
    g_qh_bulk_in = (ehci_qh *)((u8 *)g_qh_dma.virt + 128);
    g_td_pool = (ehci_td *)((u8 *)g_qh_dma.virt + 256);

    /* 建环并启动 async schedule（必须在 CONFIGFLAG/端口操作前） */
    ehci_hc_start();

    /* CONFIGFLAG=1（OS 所有端口） */
    mmio_write32(g_caplen + EHCI_OP_CONFIGFLAG, 1u);

    /* 端口电源 */
    for (u32 p = 0; p < g_nports; p++) {
        u32 po = g_caplen + EHCI_OP_PORTSC + p * EHCI_PORT_STRIDE;
        u32 ps = mmio_read32(po);
        if (g_ppc && !(ps & PORTSC_PP)) {
            mmio_write32(po, ps | PORTSC_PP);
            dkm_delay_ms(20);
        }
    }
    dkm_delay_ms(100);  /* 端口稳定 */

    /* 扫描端口，枚举 MSC */
    g_msc.ready = 0;
    for (u32 p = 0; p < g_nports; p++) {
        u32 po = g_caplen + EHCI_OP_PORTSC + p * EHCI_PORT_STRIDE;
        u32 ps = mmio_read32(po);
        log_hex("[ehci] port=", p);
        log_hex("[ehci] PORTSC=", ps);
        if (!(ps & PORTSC_CONNECT)) continue;

        if (ehci_reset_port(p) != 0) {
            g_log->warn("[ehci] port reset failed");
            continue;
        }
        dkm_delay_ms(100);
        if (ehci_enum_msc(p) != 0) {
            g_log->warn("[ehci] MSC enumeration failed");
            continue;
        }
        /* TEST UNIT READY（重试 5 次） */
        for (int i = 0; i < 5; i++) {
            if (ehci_scsi_test_unit_ready() == 0) break;
            dkm_delay_ms(100);
        }
        /* READ CAPACITY */
        u64 sectors = 0;
        u32 blk = 512;
        if (ehci_scsi_read_capacity(&sectors, &blk) != 0) {
            g_log->warn("[ehci] READ CAPACITY failed");
            sectors = 0; blk = 512;
        }
        g_msc.sector_count = sectors;
        g_msc.sector_size = blk;
        g_msc.ready = 1;
        DKM_STAT_INC(ehci_msc_found);
        log_hex("[ehci] MSC device ready, sectors=", sectors);
        log_hex("[ehci] MSC device block_size=", blk);
        break;
    }

    /* 注册 block provider */
    if (g_msc.ready && api->block && api->block->register_device) {
        struct dkm_block_device_desc desc;
        desc.name = "usb2";
        desc.sector_size = g_msc.sector_size;
        desc.sector_count = g_msc.sector_count;
        desc.ctx = 0;
        desc.read = ehci_block_read;
        desc.write = ehci_block_write;
        int index = api->block->register_device(&desc);
        log_hex("[ehci] block provider index=", (u64)(i64)index);
    } else {
        g_log->info("[ehci] no MSC device found; block provider not registered");
    }

    g_log->info("[ehci] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    if (g_mmio) {
        mmio_write32(g_caplen + EHCI_OP_USBCMD,
                     mmio_read32(g_caplen + EHCI_OP_USBCMD) & ~USBCMD_RUN);
    }
    return 0;
}