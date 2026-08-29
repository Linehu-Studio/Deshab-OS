/* DKM xHCI Driver — USB xHCI host controller + Mass Storage Class (BOT/SCSI)
 *
 * Stage 1 optional storage driver, depends on "pci" and "irq", provides "block".
 *
 * Architecture:
 *   1. PCI discovery: class=0x0C/03/30 (Serial Bus / USB / xHCI)
 *   2. MMIO mapping: 64-bit BAR0 via mm_map_mmio or HHDM fallback
 *   3. Controller init: BIOS handoff, HC reset, parse capabilities
 *   4. Ring setup: Command Ring, Event Ring, ERST, DCBAA, Scratchpad
 *   5. Port scan: detect connected devices, port reset
 *   6. USB enumeration: Enable Slot, Address Device, GET_DESCRIPTOR
 *   7. MSC protocol: BOT (Bulk-Only Transport) + SCSI commands
 *   8. Block provider: register usb0 via kernel_api.block
 *
 * Synchronous polling model (no IRQ/MSI in first version).
 * All hardware waits use TSC deadline — no infinite loops.
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

DKM_STAT_DECL(xhci_pci_found);
DKM_STAT_DECL(xhci_port_reset_ok);
DKM_STAT_DECL(xhci_msc_found);
DKM_STAT_DECL(xhci_block_reads);
DKM_STAT_DECL(xhci_block_writes);

static const char *const g_depends[] = { "pci", "irq" };
static const char *const g_provides[] = { "block" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "xhci",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 6,   /* DKM_CLASS_STORAGE */
    .stage          = 1,
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

/* ================================================================
 *  Constants — PCI, xHCI registers, TRB types, USB
 * ================================================================ */

#define XHCI_CLASS_SERIAL   0x0C
#define XHCI_SUBCLASS_USB  0x03
#define XHCI_PROGIF_XHCI   0x30

/* Capability Registers (MMIO + 0x00) */
#define XHCI_CAP_HCSPARAMS1   0x00
#define XHCI_CAP_HCSPARAMS2   0x04
#define XHCI_CAP_HCSPARAMS3   0x08
#define XHCI_CAP_HCCPARAMS1   0x0C
#define XHCI_CAP_DBOFF        0x14
#define XHCI_CAP_RTSOFF       0x18

/* Operational Registers (MMIO + cap_length) */
#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_DNCTRL        0x14
#define XHCI_OP_CRCR_LO       0x18
#define XHCI_OP_CRCR_HI       0x1C
#define XHCI_OP_DCBAAP_LO     0x30
#define XHCI_OP_DCBAAP_HI     0x34
#define XHCI_OP_CONFIG        0x38

/* Port Registers (MMIO + 0x400 + port * 0x10) */
#define XHCI_PORT_BASE_OFF   0x400
#define XHCI_PORT_STRIDE     0x10
#define XHCI_PORT_SC         0x00

/* Runtime Registers (MMIO + rtsoff) */
#define XHCI_RT_MFINDEX       0x00
#define XHCI_RT_IR0_IMAN      0x20
#define XHCI_RT_IR0_IMOD      0x24
#define XHCI_RT_IR0_ERSTSZ    0x28
#define XHCI_RT_IR0_ERSTBA_LO 0x30
#define XHCI_RT_IR0_ERSTBA_HI 0x34
#define XHCI_RT_IR0_ERDP_LO   0x38
#define XHCI_RT_IR0_ERDP_HI   0x3C

/* ERDP bits: bit3 = Event Handler Busy (软件写 1 清挂起) */
#define ERDP_EHB      (1u << 3)

/* USBCMD bits */
#define USBCMD_RS     (1u << 0)
#define USBCMD_RST   (1u << 1)
#define USBCMD_INTE  (1u << 2)

/* USBSTS bits */
#define USBSTS_HCH   (1u << 0)
#define USBSTS_EINT  (1u << 3)
#define USBSTS_CNR   (1u << 11)

/* PORTSC bits */
#define PORTSC_CCS   (1u << 0)
#define PORTSC_PED   (1u << 1)
#define PORTSC_OCA   (1u << 3)
#define PORTSC_PR    (1u << 4)
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK  0xF
/* Port Speed 字段在 PORTSC bits[13:10]（xHCI 1.2 §5.4.8）。
 * BUG-FIX: 原实现 shift=17/mask=0x7 是错的——QEMU 用 10:13，
 * 误读导致 SuperSpeed(4) 被当成 FULL(1)，EP0 MPS 错误。 */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xF

/* TRB types */
#define TRB_TYPE_NORMAL        0x01
#define TRB_TYPE_SETUP_STAGE  0x02
#define TRB_TYPE_DATA_STAGE   0x03
#define TRB_TYPE_STATUS_STAGE 0x04
#define TRB_TYPE_LINK         0x06
#define TRB_TYPE_ENABLE_SLOT  0x09
#define TRB_TYPE_DISABLE_SLOT 0x0A
#define TRB_TYPE_ADDRESS_DEV  0x0B
#define TRB_TYPE_CONFIGURE_EP 0x0C
#define TRB_TYPE_NOOP         0x17

/* Event TRB types — xHCI 1.2 spec Table 6-28 (TRB Type field [15:10]).
 * 规范值：Transfer=0x20, Command Completion=0x21, Port Status Change=0x22。
 * BUG-FIX: 原定义 0x01/0x02/0x21 与规范冲突——CMD_COMPLETE(0x21) 被
 * 误判为 PORT_CHANGE 事件消费，导致 Enable Slot 等命令事件环被提前
 * 清空，后续命令全部超时（QEMU hcd-xhci.c xhci_write_event 佐证）。 */
#define EVT_TYPE_TRANSFER     0x20   /* Transfer Event */
#define EVT_TYPE_CMD_COMPLETE 0x21   /* Command Completion Event */
#define EVT_TYPE_PORT_CHANGE 0x22   /* Port Status Change Event */

/* TRB control field helpers */
#define TRB_TYPE_SHIFT 10
#define TRB_TYPE_MASK  0x3F
#define TRB_IOC        (1u << 5)   /* Interrupt On Completion */
#define TRB_CH         (1u << 6)   /* Chain bit */
#define TRB_ENT        (1u << 7)   /* Evaluate Next TRB (for Link) */
#define TRB_ISP        (1u << 8)   /* Interrupt on Short Packet */
#define TRB_CYCLE      (1u << 0)
#define TRB_TC         (1u << 1)   /* Toggle Cycle (for Link) */

/* Setup Stage TRB direction */
#define SETUP_DIR_OUT  0
#define SETUP_DIR_IN  1

/* USB speeds */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* Endpoint types for EP Context */
#define EP_TYPE_CTRL_OUT  0
#define EP_TYPE_CTRL_IN   4
#define EP_TYPE_BULK_OUT  2
#define EP_TYPE_BULK_IN   6

/* Slot states */
#define SLOT_STATE_DISABLED     0
#define SLOT_STATE_ENABLED      1
#define SLOT_STATE_ADDRESSED    2
#define SLOT_STATE_CONFIGURED   3

/* Ring sizes */
#define CMD_RING_SIZE   256
#define EVT_RING_SIZE   256
#define XFER_RING_SIZE  256

/* xHCI MMIO mapping size (64 KB covers most controllers) */
#define XHCI_MAP_SIZE   0x10000

/* BOT (Bulk-Only Transport) signatures */
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

typedef struct {
    u64 parameter;   /* DW0-DW1 */
    u32 status;      /* DW2 */
    u32 control;     /* DW3 */
} xhci_trb;

/* TRB Ring */
typedef struct {
    struct dkm_dma_buffer dma;
    u32  size;          /* TRB count */
    u32  enqueue;       /* producer index */
    u32  ccs;           /* producer Cycle State */
} xhci_ring;

/* ERST entry */
typedef struct {
    u64 seg_addr;
    u32 seg_size;
    u32 rsvd;
} xhci_erst_entry;

/* MSC device state */
typedef struct {
    u8  slot_id;
    u8  speed;
    u8  bulk_out_ep;    /* EP number for Bulk OUT */
    u8  bulk_in_ep;     /* EP number for Bulk IN */
    u16 bulk_out_mps;   /* Max Packet Size */
    u16 bulk_in_mps;    /* Max Packet Size */
    u32 cbw_tag;        /* BOT tag counter */
    u64 sector_count;
    u32 sector_size;
    int ready;
} xhci_msc_dev;

/* ================================================================
 *  Global state
 * ================================================================ */

static const struct dkm_kernel_api *g_api;
static const struct dkm_log_api *g_log;
static volatile u8 *g_mmio;          /* MMIO base virtual address */
static u32  g_cap_length;            /* Capability register space length */
static u32  g_doorbell_off;          /* Doorbell register offset */
static u32  g_runtime_off;           /* Runtime register space offset */
static u32  g_max_slots;
static u32  g_max_ports;
static u32  g_max_intrs;
static u32  g_ctx_size;              /* 32 or 64 bytes */
static u32  g_page_size;             /* controller page size in bytes */
static u32  g_spb_count;             /* scratchpad buffer count */
static u32  g_port_base;             /* absolute MMIO offset of port register set (cap_length + 0x400) */
static u32  g_ppc;                   /* Per-Port Power Control (HCCPARAMS1 bit 3) */

static xhci_ring  g_cmd_ring;
static xhci_ring  g_evt_ring;
static u32        g_evt_ccs;         /* consumer cycle state for event ring */

static struct dkm_dma_buffer g_erst;     /* ERST DMA buffer */
static struct dkm_dma_buffer g_dcbaa;   /* DCBAA DMA buffer */
static struct dkm_dma_buffer g_sp_bufs; /* Scratchpad buffer array */
/* Scratchpad individual pages — max 32 scratchpads for safety */
static struct dkm_dma_buffer g_sp_pages[32];

/* Per-device DMA buffers */
static struct dkm_dma_buffer g_input_ctx;  /* Input Context */
static struct dkm_dma_buffer g_dev_ctx;    /* Device Context (output) */
static xhci_ring             g_ep0_ring;   /* EP0 control transfer ring */
static xhci_ring             g_bulk_out_ring;
static xhci_ring             g_bulk_in_ring;
static struct dkm_dma_buffer g_xfer_buf;   /* Data transfer bounce buffer */
static struct dkm_dma_buffer g_csw_buf;    /* CSW receive buffer */

static xhci_msc_dev g_msc;

/* PCI device location */
static u8 g_pci_bus, g_pci_dev, g_pci_func;

/* ================================================================
 *  Utility functions
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

/* ================================================================
 *  Framebuffer 阶段诊断（实机无串口时屏幕直接显示 xhci 失败点）
 *  失败时整屏填背景色 + 中央画大号两位十六进制状态码，然后 halt。
 *  码表（调用点注释中注明）：
 *    红底 0x01..0x07  基础设施失败（PCI/MMIO/caps/reset/ring/start）
 *    红底 0x11..0x14  PCI 自检细分（见 xhci_pci_diag_code）
 *    黄底 0x08        所有端口无设备可枚举
 *    黄底 0x0A..0x0F  枚举失败（slot/address/descriptor/configure/scsi）
 *  依赖 dkm_kernel_api.fb_* 字段（32bpp，与 console_fb 一致）。
 * ================================================================ */
static const u8 g_fb_font_hex[16][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
    {0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    {0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, /* A */
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, /* B */
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, /* C */
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, /* D */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, /* E */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
};

static void fb_draw_char(u32 *fb, u64 pitch, int x0, int y0, int scale,
                         u32 color, int ch) {
    const u8 *g = g_fb_font_hex[ch & 0xF];
    for (int row = 0; row < 8; row++) {
        u8 bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            for (int sy = 0; sy < scale; sy++) {
                u32 *line = (u32 *)((u8 *)fb + (u64)(y0 + row * scale + sy) * pitch);
                for (int sx = 0; sx < scale; sx++)
                    line[x0 + col * scale + sx] = color;
            }
        }
    }
}

static void fb_diag_show(const struct dkm_kernel_api *api, u32 bg, u32 code) {
    if (!api || !api->fb_address || !api->fb_width || !api->fb_height)
        return;  /* 无 framebuffer 时退化为仅串口日志 */
    u32 *fb = (u32 *)api->fb_address;
    u64 pitch = api->fb_pitch;
    u64 w = api->fb_width, h = api->fb_height;
    for (u64 y = 0; y < h; y++) {
        u32 *line = (u32 *)((u8 *)fb + y * pitch);
        for (u64 x = 0; x < w; x++) line[x] = bg;
    }
    int scale = 8;
    int cw = 8 * scale;
    int cx = (int)(w / 2) - cw;             /* 两位数字整体居中 */
    int cy = (int)(h / 2) - (8 * scale) / 2;
    u32 fg = 0xFFFFFFFFu;
    if (code >= 0x10) fb_draw_char(fb, pitch, cx, cy, scale, fg, (int)(code >> 4));
    fb_draw_char(fb, pitch, cx + cw, cy, scale, fg, (int)(code & 0xF));
    /* 停住屏幕，等待实机拍照/观察 */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* 任意位数 hex（n=1..8），低位在前填充 */
static void fb_draw_hexn(u32 *fb, u64 pitch, int x0, int y0, int scale,
                         u32 color, u32 v, int n) {
    for (int i = 0; i < n; i++) {
        int nib = (int)((v >> (4 * (n - 1 - i))) & 0xF);
        fb_draw_char(fb, pitch, x0 + i * 8 * scale, y0, scale, color, nib);
    }
}

/* PCI 自检诊断清单项（bus/dev/device_id/class/progif） */
struct usb_ctl_info {
    u32 bus;
    u32 dev;
    u32 devid;    /* PCI device ID */
    u32 cls;      /* (class<<8)|subclass */
    u32 progif;
};

/* 扩展诊断：大字标题码 + 下方逐行列出扫描到的 PCI 设备。
 * 每行：bus(1) dev(1) deviceid(4) = 左 6 hex，class(2) progif(2) = 右 4 hex。
 * scale=2 支持 16 行（640x480 完整容纳）。device ID 用于区分芯片型号：
 *   Intel 7系: xHCI=1e31, EHCI1=1e2d, EHCI2=1e26
 *   Intel 6系: EHCI1=1c2d, EHCI2=1c26 (H61 仅 1 个 EHCI)
 *   第三方 USB3: ASMedia=1b21, Renesas=1033 */
/* 诊断用，保留 */
static void fb_diag_show_usb(const struct dkm_kernel_api *api, u32 bg, u32 code,
                             const struct usb_ctl_info *usb, u32 usb_count) __attribute__((unused));
static void fb_diag_show_usb(const struct dkm_kernel_api *api, u32 bg, u32 code,
                             const struct usb_ctl_info *usb, u32 usb_count) {
    if (!api || !api->fb_address || !api->fb_width || !api->fb_height)
        return;
    u32 *fb = (u32 *)api->fb_address;
    u64 pitch = api->fb_pitch;
    u64 w = api->fb_width, h = api->fb_height;
    for (u64 y = 0; y < h; y++) {
        u32 *line = (u32 *)((u8 *)fb + y * pitch);
        for (u64 x = 0; x < w; x++) line[x] = bg;
    }
    u32 fg = 0xFFFFFFFFu;
    /* 标题大字（scale 6，两位居中，固定顶部） */
    int s = 6, cw = 8 * s;
    int cx = (int)(w / 2) - cw;
    int cy = 8;
    if (code >= 0x10) fb_draw_char(fb, pitch, cx, cy, s, fg, (int)(code >> 4));
    fb_draw_char(fb, pitch, cx + cw, cy, s, fg, (int)(code & 0xF));
    /* 信息行（scale 2，最多 16 行）：
     * 左：bus(1)+dev(1)+devid(4)=6 hex；右：class(2)+progif(2)=4 hex */
    int s2 = 2, cw2 = 8 * s2;
    int row_y = cy + 8 * s + 8;
    int total = (6 + 1 + 4) * cw2;          /* 176px */
    int xl = (int)(w / 2) - total / 2;
    int xr = xl + (6 + 1) * cw2;
    u32 max_rows = usb_count < 16 ? usb_count : 16;
    for (u32 i = 0; i < max_rows; i++) {
        u32 bd = ((usb[i].bus & 0xFF) << 4) | (usb[i].dev & 0x1F);
        fb_draw_hexn(fb, pitch, xl, row_y, s2, fg, bd, 2);
        fb_draw_hexn(fb, pitch, xl + 2 * cw2, row_y, s2, fg, usb[i].devid & 0xFFFF, 4);
        fb_draw_hexn(fb, pitch, xr, row_y, s2, fg, usb[i].cls & 0xFFFF, 2);
        fb_draw_hexn(fb, pitch, xr + 2 * cw2, row_y, s2, fg, usb[i].progif & 0xFFFF, 2);
        row_y += 8 * s2 + 4;                /* 20px/行 */
    }
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* 红底 = 基础设施失败，halt（0xFFFF0000 在 BGRA 下是纯红；
 * 原 0xFF0000FF 实际渲染为蓝——0x01 那次实机测试显示“蓝底”即此原因） */
#define FB_FATAL(api, code) fb_diag_show((api), 0xFFFF0000u, (code))
/* 红底 PCI 自检：标题码 + USB 控制器清单 */
#define FB_FATAL_show_usb(api, code) \
    fb_diag_show_usb((api), 0xFFFF0000u, (code), g_diag_usb, g_diag_usb_count)
/* 黄底 = 枚举失败，halt */
#define FB_ENUM_FAIL(code)  fb_diag_show(g_api, 0xFFFFC800u, (code))

static void xhci_zero(void *ptr, u32 len) {
    u8 *p = (u8 *)ptr;
    for (u32 i = 0; i < len; i++) p[i] = 0;
}

static void xhci_memcpy(void *dst, const void *src, u32 len) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u32 i = 0; i < len; i++) d[i] = s[i];
}

/* MMIO read/write */
static u32 mmio_read32(u32 off) {
    return *(volatile u32 *)(void *)(g_mmio + off);
}

static void mmio_write32(u32 off, u32 value) {
    *(volatile u32 *)(void *)(g_mmio + off) = value;
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

/* Memory barrier (compiler only — x86 TSO guarantees HW ordering) */
static __inline__ void xhci_mb(void) {
    __asm__ volatile("" ::: "memory");
}

/* ================================================================
 *  Phase 1: PCI discovery + MMIO mapping
 * ================================================================ */

/* PCI 自检：区分“传统 0xCF8/0xCFC 配置机制失效” vs “确实无 xHCI 控制器”。
 * 实机现象：xhci_find 全扫描失败（诊断码 0x01）——USB3 主板必然有 xHCI，
 * 扫不到只可能是 (a) 配置机制读不到任何设备 (b) 设备在异常位置 (c) 控制器
 * 是 EHCI 等非 xHCI。返回细分诊断码：
 *   0x11  bus0/dev0/func0 vendor=0xFFFF → 0xCF8/0xCFC 机制失效
 *   0x12  机制可用但全扫描无任何 USB 控制器 (class 0x0C/03)
 *   0x13  有 USB 控制器但无 prog_if=0x30（全部 EHCI/UHCI/OHCI）
 *   0x14  存在 prog_if=0x30 的 xHCI 但 xhci_find 漏匹配（防御性，理论不可能）
 * 同时把扫描到的设备填入 g_diag_usb 供屏幕显示——记录策略：
 * 第一遍收集全部 USB 控制器（class 0C/03，含 bus1+ 第三方芯片），
 * 第二遍收集其他设备（bus0 优先），总上限 16 行。
 * 关键 device ID 判读：Intel 7系 xHCI=1e31 / EHCI1=1e2d / EHCI2=1e26；
 * Intel 6系 EHCI1=1c2d / EHCI2=1c26（H61 仅 1 个）；ASMedia=1b21；
 * Renesas=1033。 */
static struct usb_ctl_info g_diag_usb[16];
static u32 g_diag_usb_count;

#define DIAG_MAX 16

static void diag_add(u32 bus, u32 dev, u32 devid, u32 cls, u32 progif) {
    if (g_diag_usb_count >= DIAG_MAX) return;
    g_diag_usb[g_diag_usb_count].bus    = bus;
    g_diag_usb[g_diag_usb_count].dev    = dev;
    g_diag_usb[g_diag_usb_count].devid  = devid;
    g_diag_usb[g_diag_usb_count].cls    = cls;
    g_diag_usb[g_diag_usb_count].progif = progif;
    g_diag_usb_count++;
}

/* 诊断用，保留（无 xHCI 场景默认不调用，可通过临时改 driver_init 调试） */
static u32 xhci_pci_diag_code(void) __attribute__((unused));
static u32 xhci_pci_diag_code(void) {
    u32 vd0 = dkm_pci_read(0, 0, 0, PCI_VENDOR_ID);
    g_diag_usb_count = 0;
    if ((vd0 & 0xffff) == 0xffff) return 0x11;

    u32 usb_total = 0, xhci_total = 0;

    /* 第一遍：USB 控制器（全 bus，含 bus1+ 第三方芯片） */
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
                u8 prog_if    = (u8)((class_reg >> 8)  & 0xff);
                u8 subclass   = (u8)((class_reg >> 16) & 0xff);
                u8 class_code = (u8)((class_reg >> 24) & 0xff);
                if (class_code == 0x0C && subclass == 0x03) {
                    usb_total++;
                    if (prog_if == 0x30) xhci_total++;
                    diag_add(bus, dev, vd2 >> 16,
                             (u32)((class_code << 8) | subclass), prog_if);
                }
            }
        }
    }

    /* 第二遍：其他设备补位（bus0 优先） */
    if (g_diag_usb_count < DIAG_MAX) {
        for (u16 bus = 0; bus < 256 && g_diag_usb_count < DIAG_MAX; bus++) {
            for (u8 dev = 0; dev < 32 && g_diag_usb_count < DIAG_MAX; dev++) {
                u32 vd = dkm_pci_read((u8)bus, dev, 0, PCI_VENDOR_ID);
                if ((vd & 0xffff) == 0xffff) continue;
                u8 header = (u8)(dkm_pci_read((u8)bus, dev, 0, PCI_HEADER) >> 16);
                u8 func_count = (header & 0x80) ? 8 : 1;
                for (u8 func = 0; func < func_count && g_diag_usb_count < DIAG_MAX; func++) {
                    u32 vd2 = dkm_pci_read((u8)bus, dev, func, PCI_VENDOR_ID);
                    if ((vd2 & 0xffff) == 0xffff) continue;
                    u32 class_reg = dkm_pci_read((u8)bus, dev, func, 0x08);
                    u8 prog_if    = (u8)((class_reg >> 8)  & 0xff);
                    u8 subclass   = (u8)((class_reg >> 16) & 0xff);
                    u8 class_code = (u8)((class_reg >> 24) & 0xff);
                    if (class_code == 0x0C && subclass == 0x03) continue; /* 已收集 */
                    diag_add(bus, dev, vd2 >> 16,
                             (u32)((class_code << 8) | subclass), prog_if);
                }
            }
        }
    }

    if (usb_total == 0) return 0x12;
    if (xhci_total > 0) return 0x14;
    return 0x13;
}

static int xhci_find(u8 *out_bus, u8 *out_dev, u8 *out_func) {
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

                if (class_code == XHCI_CLASS_SERIAL &&
                    subclass  == XHCI_SUBCLASS_USB &&
                    prog_if   == XHCI_PROGIF_XHCI) {
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

static volatile u8 *xhci_map_bar(const struct dkm_kernel_api *api, u64 bar_phys) {
    u64 bar_virt = api->hhdm_offset + bar_phys;

    /* Prefer mm_map_mmio independent window */
    if (api->mm_map_mmio) {
        void *mapped = api->mm_map_mmio(bar_phys, XHCI_MAP_SIZE);
        if (mapped) {
            g_log->info("[xhci] BAR mapped via mm_map_mmio");
            return (volatile u8 *)mapped;
        }
        g_log->warn("[xhci] mm_map_mmio failed; trying HHDM fallback");
    }

    /* Check if HHDM covers this BAR */
    if (api->mmio && api->mmio->is_mapped) {
        int level = api->mmio->is_mapped(bar_virt);
        if (level > 0) {
            g_log->info("[xhci] HHDM covers BAR; direct access");
            return (volatile u8 *)(uintptr_t)bar_virt;
        }
        /* Map via page tables */
        if (api->mmio->map_mmio) {
            int rc = api->mmio->map_mmio(bar_phys, XHCI_MAP_SIZE);
            if (rc == 0 && api->mmio->is_mapped(bar_virt) > 0) {
                g_log->info("[xhci] BAR mapped via page tables");
                return (volatile u8 *)(uintptr_t)bar_virt;
            }
        }
    }

    /* Low BAR fallback */
    if (bar_phys < 0x100000000ULL) {
        g_log->warn("[xhci] assuming HHDM covers low BAR");
        return (volatile u8 *)(uintptr_t)bar_virt;
    }

    g_log->error("[xhci] BAR MMIO unavailable");
    return (volatile u8 *)0;
}

/* ================================================================
 *  Phase 1: BIOS Handoff via Extended Capabilities
 * ================================================================ */

static void xhci_bios_handoff(void) {
    u32 hcc1 = mmio_read32(XHCI_CAP_HCCPARAMS1);
    u32 ext_ptr = (hcc1 >> 16) & 0xFFFF;
    if (!ext_ptr) return;

    /* Walk extended capabilities (offsets in 32-bit words from MMIO base) */
    u32 off = ext_ptr;
    for (int i = 0; i < 16 && off != 0; i++) {
        u32 dw0 = mmio_read32(off * 4);
        u8 id = (u8)(dw0 & 0xFF);
        u8 next = (u8)((dw0 >> 8) & 0xFF);

        if (id == 1) {
            /* USB Legacy Support Capability */
            u32 legsup = mmio_read32(off * 4 + 4);
            log_hex("[xhci] USBLEGSUP=", legsup);

            if (legsup & 0x0000FFFFu) {
                /* BIOS owns controller — request ownership */
                mmio_write32(off * 4 + 4, legsup | (1u << 16));
                u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
                while (dkm_rdtsc() < deadline) {
                    u32 cur = mmio_read32(off * 4 + 4);
                    if (!(cur & 0x0000FFFFu)) {
                        g_log->info("[xhci] BIOS handoff complete");
                        /* Clear SMI triggers */
                        mmio_write32(off * 4 + 8, 0);
                        break;
                    }
                    __asm__ volatile("pause");
                }
            }
        }

        off = next ? (off + next) : 0;
    }
}

/* ================================================================
 *  Phase 1: Controller reset and init
 * ================================================================ */

static int xhci_controller_reset(void) {
    u32 usbcmd = mmio_read32(g_cap_length + XHCI_OP_USBCMD);
    u32 usbsts = mmio_read32(g_cap_length + XHCI_OP_USBSTS);

    /* If controller is running, halt it first */
    if (!(usbsts & USBSTS_HCH)) {
        mmio_write32(g_cap_length + XHCI_OP_USBCMD, usbcmd & ~USBCMD_RS);
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
        while (dkm_rdtsc() < deadline) {
            if (mmio_read32(g_cap_length + XHCI_OP_USBSTS) & USBSTS_HCH) break;
            __asm__ volatile("pause");
        }
    }

    /* Wait for CNR to clear */
    {
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 1000;
        while (dkm_rdtsc() < deadline) {
            if (!(mmio_read32(g_cap_length + XHCI_OP_USBSTS) & USBSTS_CNR)) break;
            __asm__ volatile("pause");
        }
    }

    /* Assert HC reset */
    usbcmd = mmio_read32(g_cap_length + XHCI_OP_USBCMD);
    mmio_write32(g_cap_length + XHCI_OP_USBCMD, usbcmd | USBCMD_RST);

    /* Wait for reset to complete (RST bit clears) */
    {
        u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 2000;
        while (dkm_rdtsc() < deadline) {
            usbcmd = mmio_read32(g_cap_length + XHCI_OP_USBCMD);
            if (!(usbcmd & USBCMD_RST)) {
                g_log->info("[xhci] controller reset complete");
                return 0;
            }
            __asm__ volatile("pause");
        }
    }
    g_log->error("[xhci] controller reset timeout");
    return -1;
}

static int xhci_parse_caps(void) {
    /* Read CAPLENGTH (low byte of MMIO+0) */
    g_cap_length = mmio_read32(0) & 0xFF;

    u32 hcs1 = mmio_read32(XHCI_CAP_HCSPARAMS1);
    u32 hcs2 = mmio_read32(XHCI_CAP_HCSPARAMS2);
    u32 hcc1 = mmio_read32(XHCI_CAP_HCCPARAMS1);

    g_max_slots = hcs1 & 0xFF;
    g_max_intrs = (hcs1 >> 8) & 0x7FF;
    g_max_ports = (hcs1 >> 24) & 0xFF;

    /* Context size: CSZ bit4 of HCCPARAMS1 */
    g_ctx_size = (hcc1 & (1u << 4)) ? 64 : 32;

    /* Per-Port Power Control: PPC bit3 of HCCPARAMS1 */
    g_ppc = (hcc1 >> 3) & 1;

    /* Scratchpad buffer count from HCSPARAMS2 */
    g_spb_count = ((hcs2 >> 21) & 0x1F) | ((hcs2 >> 27) & 0x1F);

    /*
     * Port Register Set offset: absolute from MMIO base.
     * xHCI spec: "Port Register Sets are located at offset 0x400
     * from the beginning of the Operational Register Space."
     * Operational Register Space starts at offset CAPLENGTH.
     * Therefore absolute port offset = CAPLENGTH + 0x400.
     * Verified against QEMU source (XHCI_REGS_OFFSET_PORT =
     * XHCI_REGS_OFFSET_OPER + XHCI_REGS_LENGTH_OPER) and Linux
     * kernel (op_regs + PORTSC_OFFSET where op_regs = base + cap_length).
     */
    g_port_base = g_cap_length + XHCI_PORT_BASE_OFF;

    /* Doorbell and Runtime offsets — full 32-bit, clear reserved low bits */
    g_doorbell_off = mmio_read32(XHCI_CAP_DBOFF) & 0xFFFFFFF0u;
    g_runtime_off  = mmio_read32(XHCI_CAP_RTSOFF) & 0xFFFFFFE0u;

    /* Page size: PAGESIZE 寄存器 bit[n] 置位表示支持 2^(n+12) 字节页。
     * 取置位的最低 bit（xHCI 规范要求软件使用最小支持页）。
     * 原实现 (ps_reg & 0xF) 语义错误，实机多 bit 置位时会得到错误页大小。 */
    u32 ps_reg = mmio_read32(g_cap_length + XHCI_OP_PAGESIZE);
    u32 ps_bits = ps_reg & 0xFFFFu;
    u32 ps_shift = 0;
    if (ps_bits) {
        while (!(ps_bits & 1u)) { ps_bits >>= 1; ps_shift++; }
        g_page_size = 4096u << ps_shift;
    } else {
        g_page_size = 4096;
    }

    log_hex("[xhci] CAPLEN=", g_cap_length);
    log_hex("[xhci] MaxSlots=", g_max_slots);
    log_hex("[xhci] MaxPorts=", g_max_ports);
    log_hex("[xhci] MaxIntrs=", g_max_intrs);
    log_hex("[xhci] ContextSize=", g_ctx_size);
    log_hex("[xhci] PPC=", g_ppc);
    log_hex("[xhci] PortBaseOff=", g_port_base);
    log_hex("[xhci] ScratchpadBufs=", g_spb_count);
    log_hex("[xhci] DBOFF=", g_doorbell_off);
    log_hex("[xhci] RTSOFF=", g_runtime_off);
    log_hex("[xhci] PageSize=", g_page_size);

    if (g_max_slots == 0 || g_max_ports == 0) {
        g_log->error("[xhci] invalid capability parameters");
        return -1;
    }
    if (g_max_intrs == 0) {
        g_log->warn("[xhci] MaxIntrs=0 (no interrupt register sets); polling mode only");
    }
    return 0;
}

/* ================================================================
 *  Phase 2: TRB Ring management
 * ================================================================ */

static int xhci_ring_init(xhci_ring *ring, u32 size) {
    if (!g_api->dma) return -1;
    /* Allocate one page (must be 4K aligned for DMA) */
    if (g_api->dma->alloc_pages(1, 4096, 0x100000000ULL, &ring->dma) != 0)
        return -2;

    xhci_zero(ring->dma.virt, ring->dma.size);
    ring->size    = size;
    ring->enqueue = 0;
    ring->ccs     = 1;   /* Producer Cycle State starts at 1 */

    /* Append Link TRB at end of ring */
    xhci_trb *trbs = (xhci_trb *)ring->dma.virt;
    u32 link_idx = size - 1;
    trbs[link_idx].parameter = ring->dma.phys;
    trbs[link_idx].status  = 0;
    trbs[link_idx].control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC | TRB_CYCLE;

    return 0;
}

/* Enqueue a TRB onto a ring and return the enqueue index before advance */
static u32 xhci_ring_enqueue(xhci_ring *ring, u64 param, u32 status, u32 control) {
    xhci_trb *trbs = (xhci_trb *)ring->dma.virt;
    u32 idx = ring->enqueue;

    /* Set cycle bit in control */
    control &= ~TRB_CYCLE;
    if (ring->ccs) control |= TRB_CYCLE;

    trbs[idx].parameter = param;
    trbs[idx].status    = status;
    trbs[idx].control   = control;

    /* Advance enqueue */
    ring->enqueue = (idx + 1) % (ring->size - 1); /* -1 to skip Link TRB */
    /* If we wrapped, toggle CCS */
    if (idx + 1 >= ring->size - 1)
        ring->ccs ^= 1;

    return idx;
}

/* Ring a doorbell: slot_id=0 for command, otherwise endpoint doorbell。
 * Doorbell Registers 每个 4 字节（xHCI 1.2 §5.6）：offset = slot_id * 4。
 * BUG-FIX: 原实现 slot_id * 0x20 偏移错误——QEMU 门铃区 reg>>=2 后
 * reg 即 slot_id，0x20 偏移会敲到 slot 8，该 slot 无 EP → 控制器不
 * 处理 TRB → 无 Transfer Event → 轮询超时。 */
static void xhci_ring_doorbell(u32 slot_id, u32 target) {
    mmio_write32(g_doorbell_off + slot_id * 4, target);
}

/* ================================================================
 *  Phase 2: Event Ring polling
 * ================================================================ */

/* Poll event ring for a specific TRB type with TSC timeout.
 * Returns 0 on success, fills out_param/out_status/out_control.
 * Returns -1 on timeout, -2 on error completion code. */
static int xhci_poll_event(u32 expected_type, u64 timeout_ms,
                           u64 *out_param, u32 *out_status, u32 *out_control) {
    xhci_trb *trbs = (xhci_trb *)g_evt_ring.dma.virt;
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * timeout_ms;

    for (;;) {
        xhci_trb *evt = &trbs[g_evt_ring.enqueue];
        u32 ctrl = evt->control;
        u8 cycle = (u8)(ctrl & TRB_CYCLE);

        /* Check if this TRB is owned by us (consumer) */
        if (cycle == (u8)g_evt_ccs) {
            u8 type = (u8)((ctrl >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK);

            /* Process this event */
            if (type == expected_type) {
                if (out_param)  *out_param  = evt->parameter;
                if (out_status) *out_status = evt->status;
                if (out_control)*out_control= evt->control;

                /* Advance dequeue */
                g_evt_ring.enqueue = (g_evt_ring.enqueue + 1) % (g_evt_ring.size - 1);
                if (g_evt_ring.enqueue == 0) g_evt_ccs ^= 1;

                /* Update ERDP: 地址 16 字节对齐，bit0 保持 0，bit3=EHB 清挂起 */
                u64 erdp = g_evt_ring.dma.phys + (u64)g_evt_ring.enqueue * sizeof(xhci_trb);
                mmio_write64(g_runtime_off + XHCI_RT_IR0_ERDP_LO, erdp | ERDP_EHB);

                /* Check completion code */
                u32 cc = (evt->status >> 24) & 0xFF;
                if (cc != 1) {  /* 1 = Success */
                    if (cc != 0) {
                        log_hex("[xhci] event completion code=", cc);
                        return -2;
                    }
                }
                return 0;
            }

            /* Unexpected event type — handle or skip */
            if (type == EVT_TYPE_PORT_CHANGE) {
                /* Port Status Change Event: clear PORTSC change bits */
                u32 port_id = (u32)((evt->parameter >> 24) & 0xFF);
                if (port_id > 0 && port_id <= g_max_ports) {
                    u32 port_off = g_port_base + (port_id - 1) * XHCI_PORT_STRIDE + XHCI_PORT_SC;
                    /* Read PORTSC and write back to clear change bits (W1C) */
                    u32 portsc = mmio_read32(port_off);
                    mmio_write32(port_off, portsc);
                    log_hex("[xhci] port change event, port=", port_id);
                }
            } else {
                log_hex("[xhci] unexpected event type=", type);
            }
            g_evt_ring.enqueue = (g_evt_ring.enqueue + 1) % (g_evt_ring.size - 1);
            if (g_evt_ring.enqueue == 0) g_evt_ccs ^= 1;
            u64 erdp2 = g_evt_ring.dma.phys + (u64)g_evt_ring.enqueue * sizeof(xhci_trb);
            mmio_write64(g_runtime_off + XHCI_RT_IR0_ERDP_LO, erdp2 | ERDP_EHB);
            continue;
        }

        if (dkm_rdtsc() > deadline) {
            g_log->warn("[xhci] event poll timeout");
            return -1;
        }
        __asm__ volatile("pause");
    }
}

/* ================================================================
 *  Phase 2: Command submission
 * ================================================================ */

static int xhci_cmd_submit(u64 param, u32 status, u32 control,
                           u64 timeout_ms,
                           u64 *out_param, u32 *out_status, u32 *out_control) {
    /* Clear Event Ring interrupt pending */
    mmio_write32(g_runtime_off + XHCI_RT_IR0_IMAN,
                 mmio_read32(g_runtime_off + XHCI_RT_IR0_IMAN) | 2u);

    /* Enqueue command TRB */
    xhci_ring_enqueue(&g_cmd_ring, param, status, control);
    xhci_mb();

    /* Debug: verify CRCR and command ring state */
    log_hex("[xhci] CRCR=", mmio_read64(g_cap_length + XHCI_OP_CRCR_LO));
    log_hex("[xhci] cmd ring enqueue=", g_cmd_ring.enqueue);
    log_hex("[xhci] cmd ring ccs=", g_cmd_ring.ccs);

    xhci_ring_doorbell(0, 0);  /* Command ring doorbell */

    /* Debug: check USBSTS after doorbell */
    log_hex("[xhci] USBSTS after db=", mmio_read32(g_cap_length + XHCI_OP_USBSTS));

    /* Poll for Command Completion Event */
    /* Debug: dump event ring head entry */
    {
        xhci_trb *evts = (xhci_trb *)g_evt_ring.dma.virt;
        log_hex("[xhci] evt ring head=", g_evt_ring.enqueue);
        log_hex("[xhci] evt[0] ctrl=", evts[g_evt_ring.enqueue].control);
    }

    int rc = xhci_poll_event(EVT_TYPE_CMD_COMPLETE, timeout_ms,
                             out_param, out_status, out_control);

    /* Clear EINT */
    mmio_write32(g_cap_length + XHCI_OP_USBSTS,
                 mmio_read32(g_cap_length + XHCI_OP_USBSTS) | USBSTS_EINT);

    return rc;
}

/* ================================================================
 *  Phase 2: Setup controller rings + DCBAA + Scratchpad
 * ================================================================ */

static int xhci_setup_rings(void) {
    if (!g_api->dma) {
        g_log->warn("[xhci] DMA API unavailable");
        return -1;
    }

    /* Command Ring */
    if (xhci_ring_init(&g_cmd_ring, CMD_RING_SIZE) != 0) {
        g_log->error("[xhci] cmd ring alloc failed");
        return -2;
    }
    log_hex("[xhci] cmd ring phys=", g_cmd_ring.dma.phys);

    /* Event Ring */
    if (xhci_ring_init(&g_evt_ring, EVT_RING_SIZE) != 0) {
        g_log->error("[xhci] evt ring alloc failed");
        return -3;
    }
    g_evt_ccs = 1;  /* Consumer Cycle State starts at 1 */
    log_hex("[xhci] evt ring phys=", g_evt_ring.dma.phys);

    /* ERST (Event Ring Segment Table) */
    if (g_api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_erst) != 0) {
        g_log->error("[xhci] ERST alloc failed");
        return -4;
    }
    xhci_erst_entry *erst = (xhci_erst_entry *)g_erst.virt;
    xhci_zero(erst, sizeof(xhci_erst_entry));
    erst->seg_addr = g_evt_ring.dma.phys;
    erst->seg_size = EVT_RING_SIZE;

    /* DCBAA (Device Context Base Address Array) */
    u32 dcbaa_size = (g_max_slots + 1) * 8;
    u32 dcbaa_pages = (dcbaa_size + 4095) / 4096;
    if (dcbaa_pages == 0) dcbaa_pages = 1;
    if (g_api->dma->alloc_pages(dcbaa_pages, 4096, 0x100000000ULL, &g_dcbaa) != 0) {
        g_log->error("[xhci] DCBAA alloc failed");
        return -5;
    }
    xhci_zero(g_dcbaa.virt, g_dcbaa.size);
    log_hex("[xhci] DCBAA phys=", g_dcbaa.phys);

    /* Scratchpad Buffer Array (if needed) */
    if (g_spb_count > 0) {
        u32 sp_arr_size = g_spb_count * 8;
        u32 sp_arr_pages = (sp_arr_size + 4095) / 4096;
        if (sp_arr_pages == 0) sp_arr_pages = 1;
        if (g_api->dma->alloc_pages(sp_arr_pages, 4096, 0x100000000ULL, &g_sp_bufs) != 0) {
            g_log->error("[xhci] scratchpad array alloc failed");
            return -6;
        }
        xhci_zero(g_sp_bufs.virt, g_sp_bufs.size);

        /* Allocate individual scratchpad buffer pages */
        u32 sp_count = g_spb_count;
        if (sp_count > 32) sp_count = 32;  /* safety cap */
        for (u32 i = 0; i < sp_count; i++) {
            if (g_api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_sp_pages[i]) != 0) {
                g_log->error("[xhci] scratchpad page alloc failed");
                return -7;
            }
            xhci_zero(g_sp_pages[i].virt, g_sp_pages[i].size);
            /* Fill scratchpad buffer array with physical addresses */
            u64 *sp_arr = (u64 *)g_sp_bufs.virt;
            sp_arr[i] = g_sp_pages[i].phys;
        }

        /* DCBAA slot 0 points to scratchpad buffer array */
        u64 *dcbaa_entries = (u64 *)g_dcbaa.virt;
        dcbaa_entries[0] = g_sp_bufs.phys;
        log_hex("[xhci] SPB array phys=", g_sp_bufs.phys);
    }

    return 0;
}

static int xhci_controller_start(void) {
    u32 cap_off = g_cap_length;

    /* Configure Command Ring */
    u64 crcr = g_cmd_ring.dma.phys | (u64)g_cmd_ring.ccs;
    mmio_write64(cap_off + XHCI_OP_CRCR_LO, crcr);

    /* Configure DCBAAP */
    mmio_write64(cap_off + XHCI_OP_DCBAAP_LO, g_dcbaa.phys);

    /* Configure MaxSlotsEn */
    mmio_write32(cap_off + XHCI_OP_CONFIG, g_max_slots);

    /* Configure Event Ring */
    mmio_write32(g_runtime_off + XHCI_RT_IR0_ERSTSZ, 1);
    mmio_write64(g_runtime_off + XHCI_RT_IR0_ERSTBA_LO, g_erst.phys);
    u64 erdp = g_evt_ring.dma.phys;
    mmio_write64(g_runtime_off + XHCI_RT_IR0_ERDP_LO, erdp | ERDP_EHB);

    /* Enable interrupt (IMAN.IE=1) */
    mmio_write32(g_runtime_off + XHCI_RT_IR0_IMAN,
                 mmio_read32(g_runtime_off + XHCI_RT_IR0_IMAN) | (1u << 1));

    /* Enable controller interrupts + start controller */
    u32 usbcmd = mmio_read32(cap_off + XHCI_OP_USBCMD);
    usbcmd |= USBCMD_INTE | USBCMD_RS;
    mmio_write32(cap_off + XHCI_OP_USBCMD, usbcmd);

    /* Wait for HCH to clear (controller running) */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 2000;
    while (dkm_rdtsc() < deadline) {
        u32 usbsts = mmio_read32(cap_off + XHCI_OP_USBSTS);
        if (!(usbsts & USBSTS_HCH)) {
            g_log->info("[xhci] controller started");
            return 0;
        }
        __asm__ volatile("pause");
    }
    g_log->error("[xhci] controller start timeout");
    return -1;
}

/* ================================================================
 *  Phase 3: Port detection and reset
 * ================================================================ */

static int xhci_reset_port(u32 port_idx) {
    u32 port_off = g_port_base + port_idx * XHCI_PORT_STRIDE + XHCI_PORT_SC;
    u32 portsc = mmio_read32(port_off);

    /* Check if device is connected */
    if (!(portsc & PORTSC_CCS)) return -1;

    /* Already enabled? */
    if (portsc & PORTSC_PED) return 0;

    /* Clear any change bits (W1C), then assert port reset */
    mmio_write32(port_off, portsc | PORTSC_PR);

    /* Wait for PED (Port Enabled) with 500ms timeout */
    u64 deadline = dkm_rdtsc() + dkm_tsc_per_ms * 500;
    while (dkm_rdtsc() < deadline) {
        portsc = mmio_read32(port_off);
        if (portsc & PORTSC_PED) {
            /* Clear port change bits (W1C: write 1 to clear) */
            mmio_write32(port_off, portsc);
            DKM_STAT_INC(xhci_port_reset_ok);

            /* Drain any Port Status Change Events from the event ring.
             * Port reset generates a Port Status Change Event that must
             * be consumed before submitting new commands, otherwise the
             * event ring gets congested and command completion events
             * may not be delivered. */
            dkm_delay_ms(10);  /* Allow controller to post the event */
            xhci_trb *evts = (xhci_trb *)g_evt_ring.dma.virt;
            for (int drain = 0; drain < 16; drain++) {
                xhci_trb *evt = &evts[g_evt_ring.enqueue];
                u32 ctrl = evt->control;
                u8 cycle = (u8)(ctrl & TRB_CYCLE);
                if (cycle != (u8)g_evt_ccs) break;  /* No more events */
                u8 type = (u8)((ctrl >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK);
                if (type == EVT_TYPE_PORT_CHANGE) {
                    u32 port_id = (u32)((evt->parameter >> 24) & 0xFF);
                    log_hex("[xhci] drained port change event, port=", port_id);
                }
                /* Advance dequeue */
                g_evt_ring.enqueue = (g_evt_ring.enqueue + 1) % (g_evt_ring.size - 1);
                if (g_evt_ring.enqueue == 0) g_evt_ccs ^= 1;
                u64 erdp = g_evt_ring.dma.phys + (u64)g_evt_ring.enqueue * sizeof(xhci_trb);
                mmio_write64(g_runtime_off + XHCI_RT_IR0_ERDP_LO, erdp | ERDP_EHB);
            }

            return 0;
        }
        /* Check if device disconnected during reset */
        if (!(portsc & PORTSC_CCS)) return -2;
        __asm__ volatile("pause");
    }
    g_log->warn("[xhci] port reset timeout");
    return -3;
}

static u8 xhci_get_port_speed(u32 port_idx) {
    u32 port_off = g_port_base + port_idx * XHCI_PORT_STRIDE + XHCI_PORT_SC;
    u32 portsc = mmio_read32(port_off);
    return (u8)((portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);
}

/* ================================================================
 *  Phase 3: Enable Slot
 * ================================================================ */

static int xhci_enable_slot(u8 *out_slot_id) {
    /* Submit Enable Slot Command TRB */
    u32 control = (TRB_TYPE_ENABLE_SLOT << TRB_TYPE_SHIFT);
    u64 out_param = 0;
    u32 out_status = 0;
    u32 out_control = 0;

    int rc = xhci_cmd_submit(0, 0, control, 5000, &out_param, &out_status, &out_control);
    if (rc != 0) {
        g_log->error("[xhci] Enable Slot failed");
        return rc;
    }

    /* Slot ID 位于 Command Completion Event 的 TRB control[31:24]。
     * (xHCI 1.2 §6.4.2.2：CMD_COMPLETE 事件 TRB parameter = 命令 TRB 地址，
     *  slot id 在 control 高字节 —— QEMU hcd-xhci.c xhci_write_event 佐证) */
    *out_slot_id = (u8)((out_control >> 24) & 0xFF);
    log_hex("[xhci] slot_id=", *out_slot_id);
    return 0;
}

/* ================================================================
 *  Phase 3: Address Device
 * ================================================================ */

static int xhci_address_device(u8 slot_id, int bsr) {
    /* Input Context physical address */
    u64 input_ctx_phys = g_input_ctx.phys;

    u32 control = (TRB_TYPE_ADDRESS_DEV << TRB_TYPE_SHIFT);
    if (bsr) control |= (1u << 9);  /* BSR (Block Set Address) */

    /* Slot ID goes in control bits [31:24] */
    control |= ((u32)slot_id << 24);

    u64 out_param = 0;
    u32 out_status = 0;
    u32 out_control = 0;
    int rc = xhci_cmd_submit(input_ctx_phys, 0, control, 5000, &out_param, &out_status, &out_control);
    if (rc != 0) {
        g_log->error("[xhci] Address Device failed");
        return rc;
    }
    return 0;
}

/* ================================================================
 *  Phase 3: Configure Endpoint
 * ================================================================ */

static int xhci_configure_endpoint(u8 slot_id) {
    u64 input_ctx_phys = g_input_ctx.phys;
    u32 control = (TRB_TYPE_CONFIGURE_EP << TRB_TYPE_SHIFT) | ((u32)slot_id << 24);

    u64 out_param = 0;
    u32 out_status = 0;
    u32 out_control = 0;
    int rc = xhci_cmd_submit(input_ctx_phys, 0, control, 5000, &out_param, &out_status, &out_control);
    if (rc != 0) {
        g_log->error("[xhci] Configure Endpoint failed");
        return rc;
    }
    return 0;
}

/* ================================================================
 *  Phase 3: Control Transfer (EP0)
 * ================================================================ */

static int xhci_control_transfer(const u8 *setup_pkt, u8 *data, u32 data_len,
                                 u8 direction) {
    /* Setup Stage TRB */
    u64 setup_val = ((u64)setup_pkt[4] << 32) | ((u64)setup_pkt[5] << 40) |
                    ((u64)setup_pkt[6] << 48) | ((u64)setup_pkt[7] << 56);
    setup_val |= (u64)setup_pkt[0] | ((u64)setup_pkt[1] << 8) |
                 ((u64)setup_pkt[2] << 16) | ((u64)setup_pkt[3] << 24);

    u32 setup_status = 8;  /* Setup packet length */
    /* Setup Stage TRB（不设 IOC：控制传输只需在最后一个 TRB 上报完成，
     * 中间 TRB 设 IOC 会让 QEMU/实机产生多余 Transfer Event，残留事件
     * 污染后续 CMD_COMPLETE 轮询——日志表现为 "unexpected event type"） */
    u32 setup_control = (TRB_TYPE_SETUP_STAGE << TRB_TYPE_SHIFT);
    /* Setup Stage TRB 必须设置 IDT (Immediate Data, bit6)：
     * setup 包 8 字节直接内嵌在 TRB parameter 字段。
     * BUG-FIX: 原实现缺 IDT，QEMU xhci_fire_ctl_transfer 校验
     * `!(trb_setup->control & TRB_TR_IDT)` 直接返回 -1 → 无 Transfer
     * Event → 轮询超时（GET_DESCRIPTOR 失败）。 */
    setup_control |= (1u << 6);  /* TRB_TR_IDT */
    /* Set direction in TRB: bit 16 of Setup Stage TRB */
    if (direction) setup_control |= (1u << 16);

    xhci_ring_enqueue(&g_ep0_ring, setup_val, setup_status, setup_control);

    /* Data Stage TRB (if data_len > 0) */
    if (data_len > 0 && data) {
        /* Copy data to transfer buffer for OUT direction */
        if (direction == 0) {
            xhci_memcpy(g_xfer_buf.virt, data, data_len);
            __asm__ volatile("wbinvd" ::: "memory");
        }

        u64 data_phys = direction ? g_xfer_buf.phys : g_xfer_buf.phys;
        u32 data_status = data_len;
        /* Data Stage 不设 IOC（避免多余事件残留） */
        u32 data_control = (TRB_TYPE_DATA_STAGE << TRB_TYPE_SHIFT);
        if (direction) data_control |= (1u << 16);  /* DIR=IN */

        xhci_ring_enqueue(&g_ep0_ring, data_phys, data_status, data_control);
    }

    /* Status Stage TRB */
    u32 status_dir = direction ? 0 : 1;  /* opposite of data direction */
    u32 status_control = (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT) | TRB_IOC;
    if (status_dir) status_control |= (1u << 16);

    xhci_ring_enqueue(&g_ep0_ring, 0, 0, status_control);

    xhci_mb();
    xhci_ring_doorbell(g_msc.slot_id, 1);  /* EP1 = EP0+1 */

    /* Poll for Transfer Event */
    u64 timeout_ms = 5000;
    u64 out_param = 0;
    u32 out_status = 0;
    int rc = xhci_poll_event(EVT_TYPE_TRANSFER, timeout_ms,
                             &out_param, &out_status, 0);
    if (rc != 0) {
        g_log->warn("[xhci] control transfer failed");
        return rc;
    }

    /* For IN direction, copy data from DMA buffer */
    if (direction && data_len > 0 && data) {
        xhci_memcpy(data, g_xfer_buf.virt, data_len);
    }

    /* Clear EINT */
    mmio_write32(g_cap_length + XHCI_OP_USBSTS,
                 mmio_read32(g_cap_length + XHCI_OP_USBSTS) | USBSTS_EINT);

    return 0;
}

/* ================================================================
 *  Phase 3: USB Descriptor reads
 * ================================================================ */

static int xhci_get_device_descriptor(u8 *out_desc) {
    u8 setup[8];
    setup[0] = 0x80;  /* bmRequestType: Device-to-Host, Standard, Device */
    setup[1] = 0x06;  /* bRequest: GET_DESCRIPTOR */
    setup[2] = 0x00;  /* wValue low: Descriptor Index */
    setup[3] = 0x01;  /* wValue high: Device Descriptor */
    setup[4] = 0x00;  /* wIndex low */
    setup[5] = 0x00;  /* wIndex high */
    setup[6] = 0x12;  /* wLength low: 18 bytes */
    setup[7] = 0x00;  /* wLength high */

    xhci_zero(out_desc, 18);
    return xhci_control_transfer(setup, out_desc, 18, 1);
}

static int xhci_get_config_descriptor(u8 *out_desc, u32 max_len) {
    u8 setup[8];
    setup[0] = 0x80;  /* Device-to-Host, Standard, Device */
    setup[1] = 0x06;  /* GET_DESCRIPTOR */
    setup[2] = 0x00;  /* Index */
    setup[3] = 0x02;  /* Configuration Descriptor */
    setup[4] = 0x00;
    setup[5] = 0x00;
    setup[6] = (u8)(max_len & 0xFF);
    setup[7] = (u8)((max_len >> 8) & 0xFF);

    xhci_zero(out_desc, max_len);
    return xhci_control_transfer(setup, out_desc, max_len, 1);
}

/* ================================================================
 *  Phase 4: Bulk Transfer
 * ================================================================ */

static int xhci_bulk_transfer(xhci_ring *ring, u8 ep_num,
                              u8 direction,  /* 0=OUT, 1=IN */
                              const void *data, u32 data_len) {
    if (data_len == 0) return -1;

    /* For OUT, copy data to DMA buffer */
    if (direction == 0) {
        xhci_memcpy(g_xfer_buf.virt, data, data_len);
        __asm__ volatile("wbinvd" ::: "memory");
    }

    /* Enqueue Normal TRB(s) — single TRB for now (up to 64KB) */
    u32 control = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC;
    xhci_ring_enqueue(ring, g_xfer_buf.phys, data_len, control);

    xhci_mb();
    /* Doorbell target = Endpoint ID：EPn OUT→2n，EPn IN→2n+1。
     * BUG-FIX: 原实现传 EP 号（2/1），QEMU xhci_kick_ep 按 Endpoint ID
     * 索引 eps[epid-1] → 敲错端点 → 无 Transfer Event → 轮询超时。 */
    u8 db_target = (direction ? (u8)(2u * ep_num + 1) : (u8)(2u * ep_num));
    xhci_ring_doorbell(g_msc.slot_id, db_target);

    /* Poll for Transfer Event */
    u64 out_param = 0;
    u32 out_status = 0;
    int rc = xhci_poll_event(EVT_TYPE_TRANSFER, 10000,
                             &out_param, &out_status, 0);
    if (rc != 0) {
        g_log->warn("[xhci] bulk transfer failed");
        log_hex("[xhci] bulk rc=", (u64)(i64)rc);
        return rc;
    }

    /* For IN, copy data from DMA buffer */
    if (direction == 1) {
        xhci_memcpy((void *)data, g_xfer_buf.virt, data_len);
    }

    /* Clear EINT */
    mmio_write32(g_cap_length + XHCI_OP_USBSTS,
                 mmio_read32(g_cap_length + XHCI_OP_USBSTS) | USBSTS_EINT);

    return 0;
}

/* ================================================================
 *  Phase 4: BOT (Bulk-Only Transport) + SCSI
 * ================================================================ */

static int xhci_bot_send_cbw(u8 lun, u8 flags, u32 data_len,
                              const u8 *cb, u8 cb_len) {
    u8 cbw[31];
    xhci_zero(cbw, 31);

    /* Signature */
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    /* Tag (little-endian) */
    u32 tag = g_msc.cbw_tag++;
    cbw[4] = (u8)(tag & 0xFF);
    cbw[5] = (u8)((tag >> 8) & 0xFF);
    cbw[6] = (u8)((tag >> 16) & 0xFF);
    cbw[7] = (u8)((tag >> 24) & 0xFF);
    /* Data Transfer Length (little-endian) */
    cbw[8] = (u8)(data_len & 0xFF);
    cbw[9] = (u8)((data_len >> 8) & 0xFF);
    cbw[10] = (u8)((data_len >> 16) & 0xFF);
    cbw[11] = (u8)((data_len >> 24) & 0xFF);
    /* Flags */
    cbw[12] = flags;
    /* LUN */
    cbw[13] = lun;
    /* CB Length */
    cbw[14] = cb_len;
    /* Command Block */
    for (u8 i = 0; i < cb_len && i < 16; i++)
        cbw[15 + i] = cb[i];

    return xhci_bulk_transfer(&g_bulk_out_ring, g_msc.bulk_out_ep,
                              0, cbw, 31);
}

static int xhci_bot_receive_csw(void) {
    int rc = xhci_bulk_transfer(&g_bulk_in_ring, g_msc.bulk_in_ep,
                                1, g_csw_buf.virt, 13);
    if (rc != 0) return rc;

    /* Verify CSW signature */
    u8 *csw = (u8 *)g_csw_buf.virt;
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53) {
        g_log->error("[xhci] CSW signature mismatch");
        return -2;
    }

    /* CSW status: byte 12, 0=OK */
    u8 status = csw[12];
    if (status != 0) {
        log_hex("[xhci] CSW status=", status);
        return -3;
    }
    return 0;
}

/* SCSI INQUIRY */
static int xhci_scsi_inquiry(void) {
    u8 cb[16];
    xhci_zero(cb, 16);
    cb[0] = SCSI_INQUIRY;
    cb[4] = 36;  /* allocation length */

    int rc = xhci_bot_send_cbw(0, 0x80, 36, cb, 6);
    if (rc != 0) return rc;

    /* Data phase: IN 36 bytes */
    u8 inq_data[36];
    rc = xhci_bulk_transfer(&g_bulk_in_ring, g_msc.bulk_in_ep,
                            1, inq_data, 36);
    if (rc != 0) return rc;

    /* CSW */
    rc = xhci_bot_receive_csw();
    if (rc != 0) return rc;

    /* Parse INQUIRY data */
    u8 dev_type = inq_data[0] & 0x1F;
    log_hex("[xhci] INQ PeripheralType=", dev_type);
    g_log->info("[xhci] INQ Vendor:");
    g_log->info((const char *)(inq_data + 8));  /* Not null-terminated but safe for log */

    return (dev_type == 0x00) ? 0 : -1;  /* 0 = Direct Access Block */
}

/* SCSI READ CAPACITY(10) */
static int xhci_scsi_read_capacity(u64 *out_sectors, u32 *out_blk_size) {
    u8 cb[16];
    xhci_zero(cb, 16);
    cb[0] = SCSI_READ_CAPACITY10;

    int rc = xhci_bot_send_cbw(0, 0x80, 8, cb, 10);
    if (rc != 0) return rc;

    /* Data phase: IN 8 bytes */
    u8 cap_data[8];
    rc = xhci_bulk_transfer(&g_bulk_in_ring, g_msc.bulk_in_ep,
                            1, cap_data, 8);
    if (rc != 0) return rc;

    /* CSW */
    rc = xhci_bot_receive_csw();
    if (rc != 0) return rc;

    /* Parse big-endian */
    u32 max_lba = ((u32)cap_data[0] << 24) | ((u32)cap_data[1] << 16) |
                  ((u32)cap_data[2] << 8)  | (u32)cap_data[3];
    u32 blk_size = ((u32)cap_data[4] << 24) | ((u32)cap_data[5] << 16) |
                   ((u32)cap_data[6] << 8)  | (u32)cap_data[7];

    *out_sectors = (u64)max_lba + 1;  /* READ CAPACITY returns max LBA */
    *out_blk_size = blk_size;

    log_hex("[xhci] capacity sectors=", *out_sectors);
    log_hex("[xhci] block size=", blk_size);
    return 0;
}

/* SCSI TEST UNIT READY */
static int xhci_scsi_test_unit_ready(void) {
    u8 cb[16];
    xhci_zero(cb, 16);
    cb[0] = SCSI_TEST_UNIT_READY;

    int rc = xhci_bot_send_cbw(0, 0x00, 0, cb, 6);
    if (rc != 0) return rc;
    return xhci_bot_receive_csw();
}

/* SCSI READ(10) */
static int xhci_scsi_read10(u64 lba, u32 count, void *buffer) {
    u8 cb[16];
    xhci_zero(cb, 16);
    cb[0] = SCSI_READ10;
    cb[2] = (u8)((lba >> 24) & 0xFF);
    cb[3] = (u8)((lba >> 16) & 0xFF);
    cb[4] = (u8)((lba >> 8)  & 0xFF);
    cb[5] = (u8)(lba & 0xFF);
    cb[7] = (u8)((count >> 8) & 0xFF);
    cb[8] = (u8)(count & 0xFF);

    u32 data_len = count * 512;

    int rc = xhci_bot_send_cbw(0, 0x80, data_len, cb, 10);
    if (rc != 0) return rc;

    /* Data phase: IN */
    rc = xhci_bulk_transfer(&g_bulk_in_ring, g_msc.bulk_in_ep,
                            1, buffer, data_len);
    if (rc != 0) return rc;

    return xhci_bot_receive_csw();
}

/* SCSI WRITE(10) */
static int xhci_scsi_write10(u64 lba, u32 count, const void *buffer) {
    u8 cb[16];
    xhci_zero(cb, 16);
    cb[0] = SCSI_WRITE10;
    cb[2] = (u8)((lba >> 24) & 0xFF);
    cb[3] = (u8)((lba >> 16) & 0xFF);
    cb[4] = (u8)((lba >> 8)  & 0xFF);
    cb[5] = (u8)(lba & 0xFF);
    cb[7] = (u8)((count >> 8) & 0xFF);
    cb[8] = (u8)(count & 0xFF);

    u32 data_len = count * 512;

    int rc = xhci_bot_send_cbw(0, 0x00, data_len, cb, 10);
    if (rc != 0) return rc;

    /* Data phase: OUT */
    rc = xhci_bulk_transfer(&g_bulk_out_ring, g_msc.bulk_out_ep,
                            0, buffer, data_len);
    if (rc != 0) return rc;

    return xhci_bot_receive_csw();
}

/* ================================================================
 *  Phase 4+5: Block device interface
 * ================================================================ */

static int xhci_block_read(void *ctx, u64 lba, u32 count, void *buffer) {
    (void)ctx;
    if (!g_msc.ready || !buffer || count == 0) return -1;
    DKM_STAT_INC(xhci_block_reads);

    u8 *buf = (u8 *)buffer;
    while (count > 0) {
        u32 chunk = count > 16 ? 16 : count;  /* max 16 sectors (8KB) per SCSI cmd */
        int rc = xhci_scsi_read10(lba, chunk, g_xfer_buf.virt);
        if (rc != 0) {
            log_hex("[xhci] READ10 failed lba=", lba);
            return rc;
        }
        xhci_memcpy(buf, g_xfer_buf.virt, chunk * 512);
        buf   += chunk * 512;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static int xhci_block_write(void *ctx, u64 lba, u32 count, const void *buffer) {
    (void)ctx;
    if (!g_msc.ready || !buffer || count == 0) return -1;
    DKM_STAT_INC(xhci_block_writes);

    const u8 *buf = (const u8 *)buffer;
    while (count > 0) {
        u32 chunk = count > 16 ? 16 : count;
        xhci_memcpy(g_xfer_buf.virt, buf, chunk * 512);
        __asm__ volatile("wbinvd" ::: "memory");  /* Cache flush for DMA */
        int rc = xhci_scsi_write10(lba, chunk, g_xfer_buf.virt);
        if (rc != 0) {
            log_hex("[xhci] WRITE10 failed lba=", lba);
            return rc;
        }
        buf   += chunk * 512;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

/* ================================================================
 *  Phase 3: Build Input Context for Address Device
 * ================================================================ */

static void xhci_build_input_ctx_slot(u8 slot_id, u8 speed, u16 mps0, u32 port_idx) {
    (void)slot_id;
    u32 ctx_sz = g_ctx_size;
    u8 *ic = (u8 *)g_input_ctx.virt;
    xhci_zero(ic, g_input_ctx.size);

    /* Input Control Context (xHCI 1.2 §6.4.3):
     *   DWORD0 = Drop Context Flags（本次全 0，不 drop 任何上下文）
     *   DWORD1 = Add Context Flags（bit0=Slot, bit1=EP0）
     * BUG-FIX: 原实现把 Add flags 写进 DWORD0（Drop 槽），
     * QEMU/实机校验 ictl_ctx[0]==0 && ictl_ctx[1]==0x3 直接失败 CC_TRB_ERROR。 */
    u32 *icc = (u32 *)ic;
    icc[0] = 0x0;                                /* Drop Context Flags */
    icc[1] = (1u << 0) | (1u << 1);              /* Add: Slot + EP0 */

    /* Slot Context (§6.2.2):
     *   DWORD0: Route String [0:19] + Speed [20:22] + Context Entries [27:31]
     *   DWORD1: Max Exit Latency [0:7] + Root Hub Port Number [16:23]
     * BUG-FIX: 原实现 speed<<29（占用 Context Entries 位域）、MPS0 写入
     * DWORD1（规范 Slot Context 无 MPS0 字段，MPS0 在 EP0 Context）——
     * QEMU xhci_lookup_uport 读 DWORD1[16:23] 找端口，原值 port=0 查不到
     * 设备 → CC_TRB_ERROR。 */
    u32 *slot_ctx = (u32 *)(ic + ctx_sz);
    u32 route_string = 0;  /* direct-connect, no hub */
    slot_ctx[0] = route_string | ((u32)speed << 20) | (1u << 27);
    slot_ctx[1] = (port_idx + 1) << 16;  /* Root Hub Port Number (1-based) */

    /* EP0 Context starts at offset ctx_sz * 2
     * Endpoint Context (§6.2.3) 布局：
     *   DWORD1: CErr[4:5] + EP Type[6:8] + Max Packet Size[16:31]
     *   DWORD2: Dequeue Pointer Low[4:31]（bit0=DCS, bits1:3=reserved）
     *   DWORD3: Dequeue Pointer High
     *   DWORD4: Average TRB Length[0:15]
     * BUG-FIX: 原实现把 dequeue 写到 DWORD6/7——QEMU xhci_init_epctx
     * 从 DWORD2/3 读 dequeue（ctx[2]&~0xf, ctx[3]），读到 0 → ring 无
     * dequeue → kick_epctx 直接返回，TRB 永不处理 → 无 Transfer Event。 */
    u32 *ep0_ctx = (u32 *)(ic + ctx_sz * 2);
    ep0_ctx[1] = (4u << 3) | ((u32)mps0 << 16);  /* EP Type=4 (Control IN), MPS */
    u64 ep0_deq = g_ep0_ring.dma.phys | (u64)g_ep0_ring.ccs;
    ep0_ctx[2] = (u32)(ep0_deq & 0xFFFFFFF0u) | (u32)g_ep0_ring.ccs;
    ep0_ctx[3] = (u32)(ep0_deq >> 32);
    ep0_ctx[4] = 8;   /* Average TRB Length */
}

static void xhci_build_input_ctx_bulk_eps(u8 bulk_out_ep, u8 bulk_in_ep,
                                           u16 out_mps, u16 in_mps) {
    u32 ctx_sz = g_ctx_size;
    u8 *ic = (u8 *)g_input_ctx.virt;

    /* Input Control Context（Configure Endpoint 语义，xHCI 1.2 §6.4.3）：
     *   DWORD0 = Drop（0），DWORD1 = Add。
     * Configure 时 bit0=Slot 必须 Add，bit1=EP0 不可重复 Add
     * （QEMU xhci_configure_slot 校验 (ictl[1]&0x3)==0x1）。
     * 端点位用 Context Index：EPn OUT→2n，EPn IN→2n+1
     * （QEMU 从 i=2 开始遍历 Add flags，按 ictx+32*i 读 EP Context）。 */
    u32 *icc = (u32 *)ic;
    u32 out_idx = 2u * bulk_out_ep;
    u32 in_idx  = 2u * bulk_in_ep + 1;
    icc[0] = 0x0;
    icc[1] = (1u << 0) | (1u << out_idx) | (1u << in_idx);

    /* Update Slot Context: Context Entries = max Context Index
     * （DWORD0 bits[27:31]，保留 Speed/Route 原值）。 */
    u32 *slot_ctx = (u32 *)(ic + ctx_sz);
    u32 max_idx = out_idx > in_idx ? out_idx : in_idx;
    slot_ctx[0] &= ~(0x1Fu << 27);
    slot_ctx[0] |= (max_idx << 27);

    /* Bulk OUT Endpoint Context。
     * 偏移 = ctx_sz * (out_idx + 1)：input context 布局是
     * [ICC][Slot][EP0][EP1]... 每个占 1 个 ctx_sz，Context Index i 的
     * EP context 在 (i+1) 个槽处（QEMU xhci_configure_slot 用
     * ictx+32+(32*i) 读取）。BUG-FIX: 原实现 ctx_sz*out_idx 少了 ICC
     * 槽 → 全部错位 32 字节 → QEMU 读到零 EP context。 */
    u32 *out_ctx = (u32 *)(ic + ctx_sz * (out_idx + 1));
    out_ctx[1] = (EP_TYPE_BULK_OUT << 3) | ((u32)out_mps << 16);
    u64 out_deq = g_bulk_out_ring.dma.phys | (u64)g_bulk_out_ring.ccs;
    out_ctx[2] = (u32)(out_deq & 0xFFFFFFF0u) | (u32)g_bulk_out_ring.ccs;
    out_ctx[3] = (u32)(out_deq >> 32);
    out_ctx[4] = 512;  /* Average TRB Length */

    /* Bulk IN Endpoint Context (at offset ctx_sz * (in_idx + 1)) */
    u32 *in_ctx = (u32 *)(ic + ctx_sz * (in_idx + 1));
    in_ctx[1] = (EP_TYPE_BULK_IN << 3) | ((u32)in_mps << 16);
    u64 in_deq = g_bulk_in_ring.dma.phys | (u64)g_bulk_in_ring.ccs;
    in_ctx[2] = (u32)(in_deq & 0xFFFFFFF0u) | (u32)g_bulk_in_ring.ccs;
    in_ctx[3] = (u32)(in_deq >> 32);
    in_ctx[4] = 512;  /* Average TRB Length */
}

/* ================================================================
 *  Phase 3: Full device enumeration
 * ================================================================ */

static int xhci_enum_msc_device(u32 port_idx) {
    u8 speed = xhci_get_port_speed(port_idx);
    log_hex("[xhci] port speed=", speed);

    /* Default MaxPacketSize for EP0 based on speed
     * (xHCI 1.2 §4.3: Full=8? No — FS/HS EP0 通常 64，LS=8，SS=512) */
    u16 mps0 = 64;  /* Full/High-Speed default */
    if (speed == USB_SPEED_LOW)  mps0 = 8;
    if (speed == USB_SPEED_SUPER) mps0 = 512;

    /* Step 1: Enable Slot */
    u8 slot_id = 0;
    if (xhci_enable_slot(&slot_id) != 0) {
        FB_ENUM_FAIL(0x0A);  /* 黄: Enable Slot 失败 */
        return -1;
    }
    g_msc.slot_id = slot_id;
    g_msc.speed = speed;

    /* Step 1.5: Allocate per-device DMA buffers（在 DCBAA 指向输出上下文之前） */
    if (g_api->dma->alloc_pages(4, 4096, 0x100000000ULL, &g_input_ctx) != 0) return -2;
    if (g_api->dma->alloc_pages(4, 4096, 0x100000000ULL, &g_dev_ctx) != 0) return -2;
    if (g_api->dma->alloc_pages(4, 4096, 0x100000000ULL, &g_xfer_buf) != 0) return -2;
    if (g_api->dma->alloc_pages(1, 4096, 0x100000000ULL, &g_csw_buf) != 0) return -2;
    if (xhci_ring_init(&g_ep0_ring, XFER_RING_SIZE) != 0) return -2;
    if (xhci_ring_init(&g_bulk_out_ring, XFER_RING_SIZE) != 0) return -2;
    if (xhci_ring_init(&g_bulk_in_ring, XFER_RING_SIZE) != 0) return -2;

    /* DCBAA[slot_id] 必须指向输出上下文（Device Context）。
     * Address Device/Configure Endpoint 命令都会读取该地址；
     * 缺失时 QEMU/实机将 octx=0 → DMA 写失败/CC_TRB_ERROR。 */
    xhci_zero(g_dev_ctx.virt, g_dev_ctx.size);
    u64 *dcbaa = (u64 *)g_dcbaa.virt;
    dcbaa[slot_id] = g_dev_ctx.phys;
    log_hex("[xhci] DCBAA[slot]=", dcbaa[slot_id]);

    /* Step 3: Build Input Context and Address Device (BSR=1 first) */
    xhci_build_input_ctx_slot(slot_id, speed, mps0, port_idx);
    if (xhci_address_device(slot_id, 1) != 0) {
        /* BSR=1 failed, try without BSR */
        xhci_build_input_ctx_slot(slot_id, speed, mps0, port_idx);
        if (xhci_address_device(slot_id, 0) != 0) {
            FB_ENUM_FAIL(0x0B);  /* 黄: Address Device 失败（BSR=1/0 均失败） */
            return -3;
        }
    }

    /* Step 4: Get Device Descriptor */
    u8 dev_desc[18];
    if (xhci_get_device_descriptor(dev_desc) != 0) {
        g_log->warn("[xhci] GET_DESCRIPTOR(Device) failed");
        FB_ENUM_FAIL(0x0C);  /* 黄: GET_DESCRIPTOR(Device) 失败 */
        return -4;
    }

    u8 dev_class = dev_desc[4];
    u16 vid = (u16)dev_desc[8] | ((u16)dev_desc[9] << 8);
    u16 pid = (u16)dev_desc[10] | ((u16)dev_desc[11] << 8);
    u8 real_mps0 = dev_desc[7];

    log_hex("[xhci] dev class=", dev_class);
    log_hex("[xhci] VID=", vid);
    log_hex("[xhci] PID=", pid);
    log_hex("[xhci] bMaxPacketSize0=", real_mps0);

    /* Step 5: Re-address with real MPS if different */
    if (real_mps0 != mps0 && real_mps0 >= 8) {
        mps0 = real_mps0;
        xhci_build_input_ctx_slot(slot_id, speed, mps0, port_idx);
        if (xhci_address_device(slot_id, 0) != 0) {
            g_log->warn("[xhci] re-address failed");
            /* Continue anyway — device may still work */
        }
    }

    /* Step 6: Get Configuration Descriptor */
    u8 cfg_desc[255];
    if (xhci_get_config_descriptor(cfg_desc, 255) != 0) {
        g_log->warn("[xhci] GET_DESCRIPTOR(Config) failed");
        FB_ENUM_FAIL(0x0C);  /* 黄: GET_DESCRIPTOR(Config) 失败 */
        return -5;
    }

    /* Parse configuration descriptor to find MSC interface and endpoints */
    u16 total_len = (u16)cfg_desc[2] | ((u16)cfg_desc[3] << 8);
    if (total_len < 9) {
        FB_ENUM_FAIL(0x0D);  /* 黄: 配置描述符过短 */
        return -5;
    }

    u8 bulk_out_ep = 0, bulk_in_ep = 0;
    u16 bulk_out_mps = 512, bulk_in_mps = 512;
    int msc_found = 0;

    /* Walk through descriptors: config(9) + interface(9) + endpoints(7*N) */
    u32 pos = 9;  /* skip config descriptor */
    while (pos + 2 <= (u32)total_len && pos + cfg_desc[pos] <= (u32)total_len) {
        u8 desc_len = cfg_desc[pos];
        u8 desc_type = cfg_desc[pos + 1];

        if (desc_type == 4 && desc_len >= 9) {
            /* Interface Descriptor */
            u8 iface_class    = cfg_desc[pos + 5];
            u8 iface_subclass = cfg_desc[pos + 6];
            u8 iface_protocol = cfg_desc[pos + 7];

            if (iface_class == 0x08 && iface_subclass == 0x06 && iface_protocol == 0x50) {
                msc_found = 1;
                g_log->info("[xhci] MSC BOT interface found");

                /* Scan following endpoint descriptors.
                 * SuperSpeed 设备在 endpoint 后紧跟 6 字节 SS Endpoint
                 * Companion descriptor (type 0x30)——必须跳过继续找下一
                 * 个 endpoint，不能因 type!=5 提前终止（Bulk OUT 常在
                 * IN 之后且被 companion 隔开）。 */
                u32 ep_pos = pos + desc_len;
                while (ep_pos + 2 <= (u32)total_len) {
                    u8 ep_len = cfg_desc[ep_pos];
                    if (ep_len == 0 || ep_pos + ep_len > (u32)total_len) break;
                    if (cfg_desc[ep_pos + 1] == 5) {  /* endpoint descriptor */
                        u8 ep_addr   = cfg_desc[ep_pos + 2];
                        u8 ep_attr   = cfg_desc[ep_pos + 3];
                        u16 ep_mps   = (u16)cfg_desc[ep_pos + 4] | ((u16)cfg_desc[ep_pos + 5] << 8);

                        if ((ep_attr & 0x03) == 0x02) {  /* Bulk */
                            if (ep_addr & 0x80) {
                                bulk_in_ep  = ep_addr & 0x0F;
                                bulk_in_mps = ep_mps;
                            } else {
                                bulk_out_ep  = ep_addr & 0x0F;
                                bulk_out_mps = ep_mps;
                            }
                        }
                    } else if (cfg_desc[ep_pos + 1] == 4) {
                        /* 下一个 interface — 停止扫描 */
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
        g_log->warn("[xhci] no MSC BOT interface found");
        FB_ENUM_FAIL(0x0D);  /* 黄: 未找到 MSC BOT 接口/端点 */
        return -6;
    }

    log_hex("[xhci] Bulk OUT EP=", bulk_out_ep);
    log_hex("[xhci] Bulk IN  EP=", bulk_in_ep);
    log_hex("[xhci] Bulk OUT MPS=", bulk_out_mps);
    log_hex("[xhci] Bulk IN  MPS=", bulk_in_mps);

    g_msc.bulk_out_ep  = bulk_out_ep;
    g_msc.bulk_in_ep   = bulk_in_ep;
    g_msc.bulk_out_mps = bulk_out_mps;
    g_msc.bulk_in_mps  = bulk_in_mps;

    /* Step 7: Configure Endpoint */
    xhci_zero(g_input_ctx.virt, g_input_ctx.size);
    xhci_build_input_ctx_bulk_eps(bulk_out_ep, bulk_in_ep,
                                   bulk_out_mps, bulk_in_mps);
    if (xhci_configure_endpoint(slot_id) != 0) {
        g_log->warn("[xhci] Configure Endpoint failed");
        FB_ENUM_FAIL(0x0E);  /* 黄: Configure Endpoint 失败 */
        return -7;
    }

    /* Step 8: SCSI INQUIRY to verify MSC device */
    if (xhci_scsi_inquiry() != 0) {
        g_log->warn("[xhci] INQUIRY failed or not block device");
        /* Continue anyway — some devices return non-zero peripheral type */
    }

    /* Step 9: TEST UNIT READY (retry up to 5 times) */
    for (int i = 0; i < 5; i++) {
        if (xhci_scsi_test_unit_ready() == 0) break;
        dkm_delay_ms(100);
    }

    /* Step 10: READ CAPACITY */
    u64 sectors = 0;
    u32 blk_size = 512;
    if (xhci_scsi_read_capacity(&sectors, &blk_size) != 0) {
        g_log->warn("[xhci] READ CAPACITY failed");
        /* Use defaults */
        sectors = 0;
        blk_size = 512;
    }

    g_msc.sector_count = sectors;
    g_msc.sector_size  = blk_size;
    g_msc.cbw_tag      = 1;
    g_msc.ready        = 1;

    DKM_STAT_INC(xhci_msc_found);
    log_hex("[xhci] MSC device ready, sectors=", sectors);
    log_hex("[xhci] MSC device block_size=", blk_size);

    return 0;
}

/* ================================================================
 *  driver_init — main entry point
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

    g_log->info("[xhci] init begin");

    /* ---- Phase 1: PCI discovery ---- */
    u8 bus = 0, dev = 0, func = 0;
    if (xhci_find(&bus, &dev, &func) != 0) {
        g_log->info("[xhci] xHCI controller not found");
        /* BUG-FIX: 不 halt。xhci 是可选驱动（manifest required=false），
         * 无 xHCI 的平台（Intel 6 系/纯 EHCI）必须继续引导到 EHCI/DSK，
         * 否则 `for(;;)hlt` 卡死整条启动链。仅记日志，如需诊断可开
         * xhci_pci_diag_code() 打印控制器清单（见注释）。 */
        return 0;
    }

    g_pci_bus = bus; g_pci_dev = dev; g_pci_func = func;
    DKM_STAT_INC(xhci_pci_found);
    g_log->info("[xhci] xHCI controller found");
    log_hex("[xhci] bus=", bus);
    log_hex("[xhci] dev=", dev);
    log_hex("[xhci] func=", func);

    u32 vd = dkm_pci_read(bus, dev, func, PCI_VENDOR_ID);
    log_hex("[xhci] vendor=", vd & 0xffff);
    log_hex("[xhci] device=", vd >> 16);

    /* Enable PCI MEM + BUSM, disable INTx (polling mode) */
    u32 command = dkm_pci_read(bus, dev, func, PCI_COMMAND) & 0xffff;
    command |= PCI_CMD_MEM | PCI_CMD_BUSM | (1u << 10);
    dkm_pci_write(bus, dev, func, PCI_COMMAND, command);
    log_hex("[xhci] PCI command=", command);

    /* Read BAR0 (64-bit) */
    u32 bar0_lo = dkm_pci_read(bus, dev, func, PCI_BAR0);
    u32 bar1_hi = dkm_pci_read(bus, dev, func, PCI_BAR1);
    u64 bar_phys;
    if (bar0_lo & 1u) {
        g_log->warn("[xhci] BAR0 is IO space; unsupported");
        FB_FATAL(api, 0x02);  /* 红: BAR0 是 IO 空间 */
        return 0;
    }
    if ((bar0_lo & 0x6u) == 0x4u) {
        bar_phys = ((u64)bar1_hi << 32) | (u64)(bar0_lo & 0xFFFFFFF0u);
    } else {
        bar_phys = (u64)(bar0_lo & 0xFFFFFFF0u);
    }

    u32 irq_line = dkm_pci_read(bus, dev, func, PCI_IRQ_LINE) & 0xff;
    log_hex("[xhci] BAR0 phys=", bar_phys);
    log_hex("[xhci] IRQ line=", irq_line);
    log_hex("[xhci] HHDM offset=", api->hhdm_offset);

    if (!api->hhdm_offset || !bar_phys) {
        g_log->warn("[xhci] missing HHDM or BAR0; skip");
        FB_FATAL(api, 0x02);  /* 红: 缺 HHDM/BAR */
        return 0;
    }

    /* ---- Phase 1: MMIO mapping ---- */
    g_mmio = xhci_map_bar(api, bar_phys);
    if (!g_mmio) {
        g_log->error("[xhci] BAR MMIO unavailable; driver inactive");
        FB_FATAL(api, 0x03);  /* 红: BAR MMIO 映射失败 */
        return 0;
    }

    /* ---- Phase 1: Parse capabilities ---- */
    if (xhci_parse_caps() != 0) {
        g_log->error("[xhci] capability parse failed");
        FB_FATAL(api, 0x04);  /* 红: capability 解析失败 */
        return 0;
    }

    /* ---- Phase 1: BIOS Handoff ---- */
    xhci_bios_handoff();

    /* ---- Phase 1: Controller Reset ---- */
    if (xhci_controller_reset() != 0) {
        g_log->error("[xhci] controller reset failed");
        FB_FATAL(api, 0x05);  /* 红: 控制器 reset 失败 */
        return 0;
    }

    /* ---- Phase 2: Setup Rings + DCBAA + Scratchpad ---- */
    if (!api->dma) {
        g_log->warn("[xhci] DMA API unavailable");
        FB_FATAL(api, 0x06);  /* 红: DMA API 缺失 */
        return 0;
    }

    if (xhci_setup_rings() != 0) {
        g_log->error("[xhci] ring setup failed");
        FB_FATAL(api, 0x06);  /* 红: ring 初始化失败 */
        return 0;
    }

    /* ---- Phase 2: Start Controller ---- */
    if (xhci_controller_start() != 0) {
        g_log->error("[xhci] controller start failed");
        FB_FATAL(api, 0x07);  /* 红: 控制器启动失败 */
        return 0;
    }

    /* ---- Phase 3: Port Scan + Device Enumeration ---- */

    /*
     * Wait 100ms after controller start for QEMU to complete USB device
     * attachment and update port status.  Without this delay, PORTSC.CCS
     * may read 0 even though a device is physically connected.
     */
    g_log->info("[xhci] waiting 100ms for port settle...");
    dkm_delay_ms(100);

    g_msc.ready = 0;
    for (u32 port = 0; port < g_max_ports; port++) {
        u32 port_off = g_port_base + port * XHCI_PORT_STRIDE + XHCI_PORT_SC;
        u32 portsc = mmio_read32(port_off);

        /* Always log PORTSC for debugging, even if no device is connected */
        log_hex("[xhci] port=", port);
        log_hex("[xhci] PORTSC=", portsc);

        if (!(portsc & PORTSC_CCS)) {
            /*
             * If PPC (Per-Port Power Control) is supported, the port may
             * need to be powered on before CCS reflects device connection.
             * Set PP (Port Power) bit in PORTSC to enable port power.
             * If PPC=0, ports are always powered and this write is a no-op.
             */
            if (g_ppc && !(portsc & (1u << 9))) {
                g_log->info("[xhci] enabling port power");
                mmio_write32(port_off, portsc | (1u << 9));
                /* Wait briefly for power to stabilize */
                dkm_delay_ms(20);
                portsc = mmio_read32(port_off);
                log_hex("[xhci] PORTSC after PP=", portsc);
            }
            if (!(portsc & PORTSC_CCS)) continue;  /* Still no device */
        }

        g_log->info("[xhci] device connected");

        /* Port Reset */
        if (xhci_reset_port(port) != 0) {
            g_log->warn("[xhci] port reset failed; skipping");
            continue;
        }
        g_log->info("[xhci] port reset OK");

        /* Try to enumerate as MSC device */
        if (xhci_enum_msc_device(port) == 0) {
            g_log->info("[xhci] MSC enumeration successful");
            break;  /* First MSC device is enough for now */
        }

        g_log->warn("[xhci] MSC enumeration failed; trying next port");
    }

    /* ---- Phase 5: Register Block Device ---- */
    if (g_msc.ready) {
        if (api->block && api->block->register_device) {
            struct dkm_block_device_desc desc;
            desc.name         = "usb0";
            desc.sector_size  = g_msc.sector_size;
            desc.sector_count = g_msc.sector_count;
            desc.ctx          = 0;
            desc.read         = xhci_block_read;
            desc.write        = xhci_block_write;
            int index = api->block->register_device(&desc);
            log_hex("[xhci] block provider index=", (u64)(i64)index);
        } else {
            g_log->warn("[xhci] block API unavailable; provider not registered");
        }
    } else {
        g_log->info("[xhci] no MSC device found; block provider not registered");
        FB_FATAL(api, 0x08);  /* 黄: 所有端口无设备/枚举全部失败 */
    }

    g_log->info("[xhci] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    /* Halt controller if it was started */
    if (g_mmio) {
        u32 usbcmd = mmio_read32(g_cap_length + XHCI_OP_USBCMD);
        mmio_write32(g_cap_length + XHCI_OP_USBCMD, usbcmd & ~USBCMD_RS);
    }
    return 0;
}
