/* mouseInit — PS/2 鼠标初始化器（完整初始化 + IRQ12 包解码 + 共享状态）
 * DSK 直接调度的系统输入初始化器，初始化完成后返回 DSK，不驻留阻塞启动流程。
 * 最高准则：任何单步失败只记日志继续执行，绝不阻塞或崩溃首次启动。
 *
 * ==================== 初始化命令序列 ====================
 *  1. ps2_drain() 冲刷控制器残留数据；
 *  2. 0x20 读配置字节 → 清 bit5（启用 AUX 时钟）、置 bit1（启用 AUX IRQ）、
 *     置 bit6（Set2→Set1 翻译）→ 0x60 写回；
 *  3. 0xA8 启用 AUX 端口；
 *  4. 0xFF 复位鼠标 → 再 drain 最多 8 字节（ACK 0xFA / 自检 0xAA / ID 0x00）；
 *  5. 0xF6 Set Defaults → 读 ACK；
 *  6. 0xF3 + 100 设置采样率 → 两次 ACK；
 *  7. 0xE8 + 2 设置分辨率（4 counts/mm）→ 两次 ACK；
 *  8. 0xF4 启用数据报告 → 读 ACK。
 *  全程 ps2_wait_write/ps2_wait_read 100000 次超时保护，防死锁。
 *
 * ==================== IRQ12 包解码 ====================
 *  经 kernel_api.irq_register 注册 IRQ12 handler。标准 3 字节包：
 *    byte0: bit7 Y溢出 bit6 X溢出 bit5 Y符号 bit4 X符号 bit3 恒1(同步位)
 *           bit2 中键 bit1 右键 bit0 左键
 *    byte1: X 位移低 8 位；byte2: Y 位移低 8 位
 *  解码规则：首字节无同步位 → 丢弃并重新同步；溢出位置位 → 丢包计 overflow；
 *  dx/dy 按 9 位有符号合成（符号位扩展）；PS/2 Y 向上为正、屏幕 Y 向下为正，
 *  故 y -= dy；坐标累计后按 framebuffer 尺寸钳位到 [0,max_x/max_y]。
 *  handler 读 0x60 前必查状态寄存器 bit5（AUX 位）：键盘数据一律不读，
 *  留在输出缓冲中交给键盘路径，避免吞掉键盘扫描码。
 *
 * ==================== handler 存活性结论（重要） ====================
 *  DSK 的 dsk_load_elf 将所有 DSK 调度模块（mouseInit/netman/FirstInit/login/
 *  desktop）加载进同一块 1MB 静态镜像缓冲：mouseInit 返回后，下一个模块的
 *  加载会整体覆盖本镜像。因此本文件中的 IRQ12 handler 与 g_mouse_state 只在
 *  mouseInit 驻留期间有效，不能作为跨模块的长期中断处理点。
 *
 *  本文件严格维持以下安全不变式，保证注册不形成野指针执行：
 *   1) UTSM pic_remap 后从片 PIC 屏蔽字为 0xFF，IRQ12 保持屏蔽，本文件不解屏蔽；
 *   2) DSK dsk_entry 已 cli，整个 DSK 阶段（含 desktop）IF=0，无任何模块 sti；
 *  => handler 已注册进内核 g_irq_handlers[12]，但永远不会被调用；
 *     镜像被后续模块覆盖后也不存在执行风险。
 *
 *  后续集成路径（二选一，均需本目录以外的配套改动，不在本文件范围）：
 *   a) DKM 输入驱动：把本文件解码逻辑搬进内核常驻 .drv（参照 ps2kbd），
 *      由它注册 IRQ12 并解屏蔽从片 bit4，desktop 改读共享状态；
 *   b) DSK 加载器增加“常驻”标志，mouseInit 镜像不被复用后，再解屏蔽 IRQ12。
 *  启用 IRQ 驱动后，desktop 等消费者必须切换为读共享状态而非轮询端口
 *  （handler 读走 0x60 后，轮询者将读不到数据）。在此之前，FirstInit/desktop
 *  维持现有端口轮询——它们均检查 AUX 位，与本初始化完全兼容：本初始化启用
 *  数据报告后，轮询者照常读到鼠标包。
 *
 * ==================== mouse_state_t 消费契约 ====================
 *  g_mouse_state 为导出全局符号，其布局即 ABI（version 字段标识版本）。
 *  消费者先校验 magic == MOUSE_STATE_MAGIC 与 version，再读 x/y/buttons 及
 *  计数器；或调用 mouse_poll_state() 取得只读快照。
 *  注意：当前 DSK 加载器无跨模块符号解析，且镜像会被覆盖——消费方真正落地
 *  依赖上述集成路径 a/b 提供常驻内存后的地址发布（例如经 ctx->reserved 槽位
 *  传递状态指针，需与 DSK 协调 ABI）。
 */

#include "../UTSM/include/utsm/dsk.h"
#include "../UTSM/include/utsm/dkm.h"
#include "../UTSM/include/utsm/idt.h"

#define COM1 0x3F8

/* ---- PS/2 端口与状态位 ---- */
#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64
#define PS2_ST_OBF  0x01    /* 输出缓冲满 */
#define PS2_ST_IBF  0x02    /* 输入缓冲满 */
#define PS2_ST_AUX  0x20    /* 输出数据来自 AUX（鼠标） */
#define PS2_TIMEOUT 100000u /* 读写等待超时次数，防死锁 */

/* ---- 控制器 / 鼠标命令 ---- */
#define PS2_CMD_READ_CFG   0x20   /* 读配置字节 */
#define PS2_CMD_WRITE_CFG  0x60   /* 写配置字节 */
#define PS2_CMD_ENABLE_AUX 0xA8   /* 启用 AUX 端口 */
#define PS2_CMD_TO_MOUSE   0xD4   /* 下一字节发往鼠标 */
#define MOUSE_CMD_RESET    0xFF
#define MOUSE_CMD_DEFAULTS 0xF6
#define MOUSE_CMD_RATE     0xF3   /* 设置采样率，跟一个字节参数 */
#define MOUSE_CMD_RESOL    0xE8   /* 设置分辨率，跟一个字节参数 */
#define MOUSE_CMD_ENABLE   0xF4   /* 启用数据报告 */
#define MOUSE_ACK          0xFA

#define IRQ_MOUSE 12

#define MOUSE_STATE_MAGIC   0x4D4F5553u /* "MOUS" */
#define MOUSE_STATE_VERSION 1u

/* 鼠标共享状态（布局即 ABI，见文件顶部“消费契约”） */
typedef struct mouse_state {
    u32 magic;                  /* MOUSE_STATE_MAGIC，消费者校验用 */
    u32 version;                /* 布局版本，当前为 1 */
    volatile i32 x;             /* 光标 X（屏幕边界钳位后） */
    volatile i32 y;             /* 光标 Y */
    volatile u32 buttons;       /* bit0=左键 bit1=右键 bit2=中键 */
    volatile u32 packet_count;  /* 成功解码包数 */
    volatile u32 overflow_count;/* 溢出 / 失步丢弃计数 */
    i32 max_x;                  /* 钳位右边界 = fb_width - 1 */
    i32 max_y;                  /* 钳位下边界 = fb_height - 1 */
} mouse_state_t;

__attribute__((visibility("default")))
mouse_state_t g_mouse_state;

/* 3 字节包组包缓冲（仅 IRQ handler 使用） */
static u8 g_pkt[3];
static u8 g_pkt_idx;

static inline void outb(u16 port, u8 value) { __asm__ volatile("outb %0,%1"::"a"(value),"Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

static void sputc(char c) {
    for (u32 i=0;i<10000;i++) if (inb(COM1+5)&0x20) break;
    outb(COM1,(u8)c);
}
static void swrite(const char *s) { while(*s){ if(*s=='\n') sputc('\r'); sputc(*s++);} }
static void logl(const char *s) { swrite(s); swrite("\n"); }
static void logh(const char *s, u64 v) {
    swrite(s);
    for (int i = 15; i >= 0; i--) {
        u8 n = (u8)((v >> (i * 4)) & 0xF);
        sputc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
    swrite("\n");
}

/* ---- PS/2 基础等待与冲刷（全程超时保护） ---- */
static int ps2_wait_write(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (!(inb(PS2_STATUS) & PS2_ST_IBF)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static int ps2_wait_read(void) {
    for (u32 t = 0; t < PS2_TIMEOUT; t++) {
        if (inb(PS2_STATUS) & PS2_ST_OBF) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}
static void ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        if (!(inb(PS2_STATUS) & PS2_ST_OBF)) break;
        inb(PS2_DATA);
    }
}
static int ps2_cmd(u8 cmd) {
    if (ps2_wait_write() != 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

/* ---- 鼠标命令：0xD4 前缀 + 命令字节 + 等 ACK(0xFA)，返回 0 成功 ---- */
static int mouse_cmd(u8 cmd) {
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -1;
    if (ps2_wait_write() != 0) return -2;
    outb(PS2_DATA, cmd);
    if (ps2_wait_read() != 0) return -3;
    return inb(PS2_DATA) == MOUSE_ACK ? 0 : -4;
}
/* 带单字节参数的鼠标命令（0xF3 采样率 / 0xE8 分辨率），命令与参数各等一次 ACK */
static int mouse_cmd_param(u8 cmd, u8 param) {
    int rc = mouse_cmd(cmd);
    if (rc != 0) return rc;
    if (ps2_cmd(PS2_CMD_TO_MOUSE) != 0) return -5;
    if (ps2_wait_write() != 0) return -6;
    outb(PS2_DATA, param);
    if (ps2_wait_read() != 0) return -7;
    return inb(PS2_DATA) == MOUSE_ACK ? 0 : -8;
}

/* ---- IRQ12 中断处理：读数据前必查 AUX 位，键盘数据一律不读。
 *     返回 0 = 由内核发送 EOI（UTSM idt.c irq_handler_t 约定）。 ---- */
static int mouse_irq12_handler(u8 irq) {
    (void)irq;
    for (u32 n = 0; n < 8; n++) {              /* 有界读取，防异常硬件下 IRQ 内死循环 */
        u8 st = inb(PS2_STATUS);
        if (!(st & PS2_ST_OBF)) break;         /* 无数据 */
        if (!(st & PS2_ST_AUX)) break;         /* 键盘数据：留给键盘路径 */
        u8 data = inb(PS2_DATA);

        if (g_pkt_idx == 0 && !(data & 0x08)) {/* 首字节缺同步位：丢字节重新同步 */
            g_mouse_state.overflow_count++;
            continue;
        }
        g_pkt[g_pkt_idx++] = data;
        if (g_pkt_idx < 3) continue;
        g_pkt_idx = 0;

        u8 f = g_pkt[0];
        if (f & 0xC0) {                        /* X/Y 溢出位：丢包 */
            g_mouse_state.overflow_count++;
            continue;
        }
        /* 9 位有符号位移合成：byte0 bit4/bit5 为 X/Y 符号位，扩展进第 9 位 */
        i32 dx = (i32)g_pkt[1] - ((f & 0x10) ? 256 : 0);
        i32 dy = (i32)g_pkt[2] - ((f & 0x20) ? 256 : 0);
        i32 nx = g_mouse_state.x + dx;
        i32 ny = g_mouse_state.y - dy;         /* PS/2 Y 向上为正，屏幕 Y 向下为正 */
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > g_mouse_state.max_x) nx = g_mouse_state.max_x;
        if (ny > g_mouse_state.max_y) ny = g_mouse_state.max_y;
        g_mouse_state.x = nx;
        g_mouse_state.y = ny;
        g_mouse_state.buttons = f & 0x07u;
        g_mouse_state.packet_count++;
    }
    return 0;
}

/* ---- 只读查询接口：把当前状态快照拷贝给调用者，返回 0 成功 ---- */
__attribute__((visibility("default")))
int mouse_poll_state(mouse_state_t *out) {
    if (!out) return -1;
    if (g_mouse_state.magic != MOUSE_STATE_MAGIC) return -2;
    out->magic          = g_mouse_state.magic;
    out->version        = g_mouse_state.version;
    out->x              = g_mouse_state.x;
    out->y              = g_mouse_state.y;
    out->buttons        = g_mouse_state.buttons;
    out->packet_count   = g_mouse_state.packet_count;
    out->overflow_count = g_mouse_state.overflow_count;
    out->max_x          = g_mouse_state.max_x;
    out->max_y          = g_mouse_state.max_y;
    return 0;
}

static void mouse_state_reset(u64 fb_w, u64 fb_h) {
    if (!fb_w) fb_w = 1024;   /* 无 framebuffer 信息时的兜底边界 */
    if (!fb_h) fb_h = 768;
    g_mouse_state.magic = MOUSE_STATE_MAGIC;
    g_mouse_state.version = MOUSE_STATE_VERSION;
    g_mouse_state.max_x = (i32)fb_w - 1;
    g_mouse_state.max_y = (i32)fb_h - 1;
    g_mouse_state.x = (i32)fb_w / 2;
    g_mouse_state.y = (i32)fb_h / 2;
    g_mouse_state.buttons = 0;
    g_mouse_state.packet_count = 0;
    g_mouse_state.overflow_count = 0;
    g_pkt_idx = 0;
}

__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx) {
    logl("[mouseInit] boot");

    /* 1. 冲刷残留数据 */
    ps2_drain();

    /* 2. 配置字节：bit5=0（启用 AUX 时钟） bit1=1（启用 AUX IRQ） bit6=1（翻译） */
    {
        int cfg_ok = 0;
        u8 cfg = 0;
        if (ps2_cmd(PS2_CMD_READ_CFG) == 0 && ps2_wait_read() == 0) {
            cfg = inb(PS2_DATA);
            cfg &= (u8)~0x20u;
            cfg |= 0x02u | 0x40u;
            if (ps2_cmd(PS2_CMD_WRITE_CFG) == 0 && ps2_wait_write() == 0) {
                outb(PS2_DATA, cfg);
                cfg_ok = 1;
            }
        }
        if (cfg_ok) logh("[mouseInit] cfg byte=", cfg);
        else logl("[mouseInit] cfg byte access failed, continue");
    }

    /* 3. 启用 AUX 端口 */
    if (ps2_cmd(PS2_CMD_ENABLE_AUX) != 0) logl("[mouseInit] enable AUX timeout, continue");

    /* 4. 复位鼠标：0xFF → 再 drain 最多 8 字节（ACK / 自检 0xAA / ID 0x00） */
    {
        int rc = mouse_cmd(MOUSE_CMD_RESET);
        if (rc != 0) logh("[mouseInit] reset rc=", (u64)(i64)rc);
        for (int i = 0; i < 8; i++) {
            if (ps2_wait_read() != 0) break;
            inb(PS2_DATA);
        }
    }

    /* 5-8. Set Defaults → 采样率 100 → 分辨率 2 → 启用数据报告，逐步验 ACK */
    {
        int rc;
        rc = mouse_cmd(MOUSE_CMD_DEFAULTS);
        if (rc != 0) logh("[mouseInit] set defaults rc=", (u64)(i64)rc);
        rc = mouse_cmd_param(MOUSE_CMD_RATE, 100);
        if (rc != 0) logh("[mouseInit] sample rate rc=", (u64)(i64)rc);
        rc = mouse_cmd_param(MOUSE_CMD_RESOL, 2);
        if (rc != 0) logh("[mouseInit] resolution rc=", (u64)(i64)rc);
        rc = mouse_cmd(MOUSE_CMD_ENABLE);
        if (rc != 0) logh("[mouseInit] enable reporting rc=", (u64)(i64)rc);
        else logl("[mouseInit] data reporting enabled");
    }

    /* 共享状态初始化：边界取自 boot context 的 framebuffer 尺寸 */
    mouse_state_reset(ctx ? ctx->framebuffer_width : 0,
                      ctx ? ctx->framebuffer_height : 0);

    /* 注册 IRQ12 handler：仅注册，按安全不变式不解屏蔽 PIC、不 sti
     * （见文件顶部“handler 存活性结论”）。 */
    {
        const dkm_kernel_api *api = (ctx && (ctx->flags & DSK_BOOT_FLAG_DKM_READY))
                                  ? (const dkm_kernel_api *)(usize)ctx->dkm_kernel_api : 0;
        if (api && api->irq_register) {
            int rc = api->irq_register(IRQ_MOUSE, (void *)mouse_irq12_handler);
            if (rc == 0) logl("[mouseInit] IRQ12 handler registered (PIC line kept masked)");
            else logh("[mouseInit] irq_register rc=", (u64)(i64)rc);
        } else {
            logl("[mouseInit] kernel api unavailable, IRQ12 not registered");
        }
    }

    logl("[mouseInit] init complete");
    return;
}
