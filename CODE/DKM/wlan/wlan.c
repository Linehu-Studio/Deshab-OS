/* DKM WLAN Driver - wireless netdev framework (stage 3, optional)
 *
 * 阶段 1（本文件现状）：
 *  - PCI 全总线扫描 class 02:80（无线网络控制器），识别常见芯片家族
 *    （Intel AX2xx/72xx、Realtek RTL8x、Atheros AR9x、Broadcom BCM43xx）。
 *  - 检测到真实硬件 → 注册 wireless netdev（attest net_wlan → NET-E01 通过）。
 *    mac 暂为全零：原生芯片后端（NVM/EEPROM 读取）未实现，且数据面回调
 *    全部走 panic —— 零 mac 不会泄露到链路上。
 *  - scan/connect/disconnect 回调 = 严格错误策略 panic 骨架：
 *    原生芯片驱动（ath9k/iwlwifi/rtw 路线）与 linuxguest 后端未落地，
 *    调用即莲花 panic（NETWL-E10/E11），坐标清晰，不伪装成功。
 *  - QEMU（无 wlan 卡）不注册任何设备 → attest MISSING 由 FUCK
 *    [attest] require_net_wlan=0 白名单豁免（设计内，见 CLAUDE.md §3）。
 *
 * 阶段 2（路线图）：芯片家族原生后端逐家实现；linuxguest 后端
 * （Linux guest 加载发行驱动 → 扫描/关联状态经共享内存回传）作为
 * 芯片覆盖面不足时的兜底数据源。
 */

#include "../dkm_shared.h"
#include "../dkm_instr.h"

static const char *const g_depends[] = {"pci"};
static const char *const g_provides[] = {"wlan"};
__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC,.abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="wlan",.version="0.1.0",.vendor="Deshab",
    .driver_class=9,.stage=3,.flags=0,.priority=0,
    .depends=g_depends,.depends_count=1,
    .provides=g_provides,.provides_count=1,.min_kernel_abi=1,
};

/* ---- 已知无线芯片 device ID 表（记录芯片名；原生后端按家族落地） ---- */
struct wlan_device_id {
    u16 vendor;
    u16 did;
    const char *name;
    const char *family;   /* 目标原生后端：ath9k/iwlwifi/rtw/brcmfmac */
};

static const struct wlan_device_id g_wlan_ids[] = {
    /* Intel (8086) — iwlwifi 家族 */
    {0x8086, 0x2723, "AX200",    "iwlwifi"},
    {0x8086, 0x2725, "AX210",    "iwlwifi"},
    {0x8086, 0x51F1, "AX211",    "iwlwifi"},
    {0x8086, 0x02F0, "AX201",    "iwlwifi"},
    {0x8086, 0xA0F0, "AX201-LP", "iwlwifi"},
    {0x8086, 0x24FD, "8265",     "iwlwifi"},
    {0x8086, 0x24F3, "8260",     "iwlwifi"},
    {0x8086, 0x095A, "7265",     "iwlwifi"},
    {0x8086, 0x24FB, "3168",     "iwlwifi"},
    /* Realtek (10EC) — rtw 家族 */
    {0x10EC, 0x8852, "RTL8852AE","rtw89"},
    {0x10EC, 0xC822, "RTL8822CE","rtw88"},
    {0x10EC, 0xC82F, "RTL8822CE","rtw88"},
    {0x10EC, 0x8821, "RTL8821AE","rtw88"},
    {0x10EC, 0x818B, "RTL8192EE","rtw88"},
    /* Atheros (168C) — ath9k 家族（无固件 blob，原生后端首选路线） */
    {0x168C, 0x002B, "AR9285",   "ath9k"},
    {0x168C, 0x002D, "AR9287",   "ath9k"},
    {0x168C, 0x002E, "AR9287-2", "ath9k"},
    {0x168C, 0x0034, "AR9462",   "ath9k"},
    {0x168C, 0x0036, "AR9565",   "ath9k"},
    {0x168C, 0x003C, "AR9462-2", "ath9k"},
    /* Broadcom (14E4) — brcmfmac 家族 */
    {0x14E4, 0x43A0, "BCM4360",  "brcmfmac"},
    {0x14E4, 0x4365, "BCM43142", "brcmfmac"},
    {0x14E4, 0x43B1, "BCM4352",  "brcmfmac"},
    {0x14E4, 0x43B2, "BCM4352-2","brcmfmac"},
};

/* ---- PCI 访问（与 e1000 相同的 CF8/CFC 端口方式） ---- */
#define PCI_ADDR_PORT 0xCF8u
#define PCI_DATA_PORT 0xCFCu

static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

static u32 pci_read(u8 bus, u8 dev, u8 func, u8 reg) {
    u32 addr = (1u << 31) | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)func << 8) | (reg & 0xFCu);
    outl(PCI_ADDR_PORT, addr);
    return inl(PCI_DATA_PORT);
}

/* ---- 扫描结果：阶段 1 无数据（scan 回调 panic），仅保留注册骨架 ---- */
static const struct wlan_device_id *g_found;   /* 命中的芯片表项 */
static u8 g_found_bus, g_found_dev, g_found_func;

/* ---- 串口直出助手（e1000 同款） ---- */
#define COM1 0x3F8

static void raw_log(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            dkm_serial_wait_tx(COM1);
            dkm_outb(COM1, '\r');
        }
        dkm_serial_wait_tx(COM1);
        dkm_outb(COM1, (u8)c);
    }
}

static void raw_hex(const char *prefix, u64 v) {
    raw_log(prefix);
    static const char hx[] = "0123456789abcdef";
    char buf[19]; buf[0]='0'; buf[1]='x';
    for (int i = 0; i < 16; i++) buf[2+i] = hx[(v >> ((15-i)*4)) & 0xf];
    buf[18] = 0;
    raw_log(buf);
    raw_log("\n");
}

static void zero_bytes(void *p, u64 n) {
    u8 *b = (u8 *)p;
    while (n--) *b++ = 0;
}

/* ---- 严格错误策略骨架：原生/网守后端未落地，调用即 panic ---- */
static struct dkm_log_api *g_log;

static void wlan_panic(const char *sym, const char *detail) {
    g_log->panic(sym);
    (void)detail;
    for (;;) __asm__ volatile("cli; hlt");
}

static int wlan_scan_start(void *ctx) {
    (void)ctx;
    wlan_panic("NETWL-E10 WIFI SCAN NOT IMPLEMENTED",
               "native chip backend pending (ath9k/iwlwifi/rtw) - linuxguest backend pending");
    return -1;
}

static int wlan_scan_count(void *ctx) {
    (void)ctx;
    wlan_panic("NETWL-E10 WIFI SCAN NOT IMPLEMENTED",
               "native chip backend pending");
    return 0;
}

static int wlan_scan_result(void *ctx, u32 n, struct dkm_net_scan_result *out) {
    (void)ctx; (void)n; (void)out;
    wlan_panic("NETWL-E10 WIFI SCAN NOT IMPLEMENTED",
               "native chip backend pending");
    return -1;
}

static int wlan_connect(void *ctx, const char *ssid, const char *password) {
    (void)ctx; (void)ssid; (void)password;
    wlan_panic("NETWL-E11 WLAN CONNECT NOT IMPLEMENTED",
               "802.11 auth/assoc + WPA handshake pending (native backend)");
    return -1;
}

static int wlan_disconnect(void *ctx) {
    (void)ctx;
    wlan_panic("NETWL-E12 WLAN DISCONNECT NOT IMPLEMENTED",
               "native chip backend pending");
    return -1;
}

static int wlan_is_wireless(void *ctx) {
    (void)ctx;
    return 1;
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    g_log = (struct dkm_log_api *)api->log;
    dkm_instr_init(api);
    g_log->info("[wlan] driver_init entered");

    /* PCI 全总线扫描：class 02 subclass 80 = 无线网络控制器。
     * 匹配已知 device ID 表记录芯片家族；未匹配但 class 命中的也
     * 登记（未知芯片 → 原生后端需新增设备表项）。 */
    for (u32 bus = 0; bus < 256; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u8 func = 0;
            u32 vd = pci_read((u8)bus, dev, func, 0x00);
            if ((vd & 0xffff) == 0xffff) continue;          /* 空设备 */
            u32 class_reg = pci_read((u8)bus, dev, func, 0x08);
            u8 class_code = (u8)(class_reg >> 24);
            u8 subclass = (u8)((class_reg >> 16) & 0xff);
            if (class_code != 0x02 || subclass != 0x80) continue;  /* 非无线控制器 */

            u16 vendor = (u16)(vd & 0xffff);
            u16 did = (u16)(vd >> 16);
            const struct wlan_device_id *hit = 0;
            for (u32 i = 0; i < sizeof(g_wlan_ids)/sizeof(g_wlan_ids[0]); i++) {
                if (g_wlan_ids[i].vendor == vendor && g_wlan_ids[i].did == did) {
                    hit = &g_wlan_ids[i]; break;
                }
            }
            g_found_bus = (u8)bus; g_found_dev = dev; g_found_func = func;

            g_log->info("[wlan] wireless controller detected:");
            g_log->info(hit ? hit->name : "(unknown chip)");
            g_log->info(hit ? hit->family : "unknown-family");
            g_found = hit;

            /* 多功能设备继续扫 func>0；命中即注册并返回（单 wlan 卡场景） */
            if (api->net && api->net->register_device) {
                struct dkm_net_device_desc netdev;
                zero_bytes(&netdev, sizeof(netdev));
                netdev.name = "wlan0";
                /* mac 全零：原生芯片后端（NVM/OTP 读取）未实现。
                 * 数据面回调全 panic —— 零 mac 不会出现在链路上。 */
                netdev.flags = 0;   /* 无 LINK_UP/TX_READY：后端未落地 */
                netdev.ctx = 0;
                netdev.scan_start = wlan_scan_start;
                netdev.scan_count = wlan_scan_count;
                netdev.scan_result = wlan_scan_result;
                netdev.is_wireless = wlan_is_wireless;
                netdev.connect = wlan_connect;
                netdev.disconnect = wlan_disconnect;
                int net_index = api->net->register_device(&netdev);
                raw_hex("[wlan] wireless netdev registered, index=", (u64)(i32)net_index);
            } else {
                g_log->warn("[wlan] net API unavailable");
            }
            return 0;
        }
    }

    g_log->info("[wlan] no wireless controller found (attest: net_wlan exempted)");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    g_found = 0;
    return 0;
}
