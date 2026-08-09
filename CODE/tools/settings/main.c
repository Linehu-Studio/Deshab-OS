/* Deshab Settings - 系统设置工具
 *
 * 内核页 (KERNEL):  查看/编辑 FUCK 内核配置（boot/drivers/dsk/debug/utsm 常用键），
 *                   行保留方式序列化（注释与未暴露键不受影响），写回
 *                   /system/deshab64/FUCK（缺失时回退根目录 FUCK）。
 * 网络页 (NETWORK): 查看/编辑网络配置，全量重写根目录 NETCONF.CNF（netman 读取）。
 * 设备页 (DEVICES): 实时枚举内核 netdev 注册表，选择连接设备，
 *                   WiFi 扫描结果查看与 SSID 选择（选择写入 NETCONF.CNF）。
 *
 * 快捷键: F1 内核页  F2 网络页  F3 设备页  Tab 切页  F5 保存  F6 重载  Esc 退出
 *         Up/Down 选择字段  Space/Enter/Left/Right 切换布尔/枚举/整数
 *         文本字段选中后直接键入编辑，Backspace 删除
 *         设备页: S 扫描WiFi  Enter 选择设备/SSID
 * 鼠标:   点击 tab / 字段行 / 按钮
 *
 * 注意: 配置修改在下次启动后生效（FUCK 由 Limine boot module 加载，
 *       NETCONF.CNF 由 netman.elf 在启动时读取）。
 */

#include "../../firstInit/ascii_bitmaps.c"
#include "../desktop_app.h"
#include "../fat32_io.h"

/* ---- Net API 类型（与 dkm_net_api 布局一致，偏移访问） ---- */
typedef u32 (*net_device_count_fn)(void);
typedef int (*net_device_info_fn)(u32 index, void *out);
typedef int (*net_scan_start_fn)(u32 index);
typedef int (*net_scan_count_fn)(u32 index);
typedef int (*net_scan_result_fn)(u32 index, u32 n, void *out);
typedef int (*net_is_wireless_fn)(u32 index);

#define NM_NET_F_LINK_UP   (1u << 0)
#define NM_NET_F_TX_READY  (1u << 1)
#define NM_NET_F_RX_READY  (1u << 2)
#define NM_NET_F_WIRELESS  (1u << 3)

#define NM_SEC_OPEN  0u
#define NM_SEC_WEP   1u
#define NM_SEC_WPA   2u
#define NM_SEC_WPA2  3u
#define NM_SEC_WPA3  4u

typedef struct {
    const char *name;
    u8 mac[6];
    u32 flags;
} nm_device_info;

typedef struct {
    char ssid[33];
    u8 bssid[6];
    u8 channel;
    signed char rssi;
    u8 security;
} nm_scan_result;

/* ---- 字段模型 ---- */
typedef enum { FT_BOOL = 0, FT_INT, FT_TEXT, FT_CHOICE } sftype;

typedef struct {
    const char *section;    /* INI section */
    const char *key;        /* 键名（同时作显示名） */
    sftype type;
    const char *choices;    /* FT_CHOICE: "|" 分隔，如 "dhcp|static" */
    int min, max;           /* FT_INT 范围 */
    char value[48];
    int dirty;
} sfield;

/* ---- 内核页字段（FUCK） ---- */
static sfield g_kfields[] = {
    /* boot */
    { "boot",    "dev_mode",           FT_BOOL, 0, 0, 0, "1", 0 },
    { "boot",    "selftest",           FT_BOOL, 0, 0, 0, "1", 0 },
    { "boot",    "vmm",                FT_BOOL, 0, 0, 0, "1", 0 },
    { "boot",    "linux_guest",        FT_BOOL, 0, 0, 0, "1", 0 },
    /* drivers */
    { "drivers", "e1000",              FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "virtio_net",         FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "nvme",               FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "ahci",               FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "apic_route",         FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "apic_route_legacy",  FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "nvme_msi",           FT_BOOL, 0, 0, 0, "1", 0 },
    { "drivers", "ath9k",              FT_BOOL, 0, 0, 0, "0", 0 },
    /* dsk */
    { "dsk",     "show_logo",          FT_BOOL, 0, 0, 0, "1", 0 },
    { "dsk",     "skip_login",         FT_BOOL, 0, 0, 0, "1", 0 },
    { "dsk",     "mouse_init",         FT_BOOL, 0, 0, 0, "1", 0 },
    { "dsk",     "netman",             FT_BOOL, 0, 0, 0, "1", 0 },
    { "dsk",     "default_shell",      FT_TEXT, 0, 0, 0, "cmd", 0 },
    /* debug */
    { "debug",   "enabled",            FT_BOOL, 0, 0, 0, "1", 0 },
    { "debug",   "log_level",          FT_INT,  0, 0, 5, "1", 0 },
    { "debug",   "net_trace",          FT_BOOL, 0, 0, 0, "0", 0 },
    /* utsm 算法参数 */
    { "utsm",    "max_segments",          FT_TEXT, 0, 0, 0, "1024", 0 },
    { "utsm",    "max_capabilities",      FT_TEXT, 0, 0, 0, "256", 0 },
    { "utsm",    "max_pckc_keys",         FT_TEXT, 0, 0, 0, "8", 0 },
    { "utsm",    "dirty_shard_pages",     FT_TEXT, 0, 0, 0, "1024", 0 },
    { "utsm",    "arena_size_mb",         FT_TEXT, 0, 0, 0, "64", 0 },
    { "utsm",    "drr_emergency_pool_kb", FT_TEXT, 0, 0, 0, "64", 0 },
    { "utsm",    "drr_recovery_log_size", FT_TEXT, 0, 0, 0, "4096", 0 },
    { "utsm",    "drr_heartbeat_interval", FT_TEXT, 0, 0, 0, "0", 0 },
};
#define KFIELD_COUNT (sizeof(g_kfields)/sizeof(g_kfields[0]))

/* ---- 网络页字段（NETCONF.CNF） ---- */
static sfield g_nfields[] = {
    { "network", "mode",     FT_CHOICE, "dhcp|static|disabled", 0, 0, "dhcp", 0 },
    { "network", "device",   FT_TEXT,   0,             0, 0, "auto", 0 },
    { "network", "ip",       FT_TEXT,   0,             0, 0, "dhcp", 0 },
    { "network", "gateway",  FT_TEXT,   0,             0, 0, "auto", 0 },
    { "network", "dns",      FT_TEXT,   0,             0, 0, "auto", 0 },
    { "network", "name",     FT_TEXT,   0,             0, 0, "", 0 },
    { "network", "password", FT_TEXT,   0,             0, 0, "", 0 },
};
#define NFIELD_COUNT (sizeof(g_nfields)/sizeof(g_nfields[0]))
#define NF_DEVICE 1   /* g_nfields 中 device 字段索引 */
#define NF_NAME   5   /* g_nfields 中 name 字段索引 */

/* ---- 全局状态 ---- */
static da_app_context g_ac;
static da_cursor g_cursor;
static da_mouse g_mouse;

static int g_page = 0;          /* 0=kernel 1=network 2=devices */
static int g_sel = 0;           /* 当前页选中字段 */
static int g_scroll = 0;        /* 当前页滚动偏移 */
static int g_sel_saved[3] = { 0, 0, 0 };
static int g_scroll_saved[3] = { 0, 0, 0 };

static char g_status[96];       /* 状态栏消息 */
static u32  g_status_color = DA_SUCCESS;
static int  g_fuck_loaded = 0;  /* FUCK 原文是否读到 */
static int  g_net_loaded = 0;

/* 配置文本缓冲 */
static char g_fuck_text[12288];   /* FUCK 原文 */
static u32  g_fuck_len = 0;
static char g_fuck_out[16384];    /* FUCK 序列化输出 */
static char g_conf_text[4096];    /* NETCONF 原文 */
static char g_conf_out[2048];     /* NETCONF 序列化输出 */

/* ---- Net API 函数指针 ---- */
static net_device_count_fn g_net_device_count;
static net_device_info_fn  g_net_device_info;
static net_scan_start_fn   g_net_scan_start;
static net_scan_count_fn   g_net_scan_count;
static net_scan_result_fn  g_net_scan_result;
static net_is_wireless_fn  g_net_is_wireless;

/* ---- 设备页状态 ---- */
#define DEV_MAX  8
#define SCAN_MAX 32

static nm_device_info g_devs[DEV_MAX];
static int g_dev_count = 0;
static nm_scan_result g_scans[SCAN_MAX];
static int g_scan_count = 0;
static int g_scan_dev_idx = -1;  /* 上次扫描的设备索引 */
static int g_net_avail = 0;      /* net API 是否可用 */

/* 设备页 item 布局:
 * 0..g_dev_count-1              : 设备行
 * g_dev_count                   : 分隔线 (不可选)
 * g_dev_count+1..+g_scan_count  : 扫描结果行
 * g_dev_count+g_scan_count+1    : SCAN 按钮
 * g_dev_count+g_scan_count+2    : SELECT 按钮
 */
#define DEV_ITEM_SCAN_BTN (-1)
#define DEV_ITEM_SEL_BTN  (-2)

/* ---- 小工具 ---- */
static int s_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void s_strcpy(char *d, const char *s, int cap) {
    int i = 0;
    for (; i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static int s_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int s_streqn(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 0; }
    return 1;
}
static void set_status(const char *msg, u32 color) {
    s_strcpy(g_status, msg, sizeof(g_status));
    g_status_color = color;
}

/* int -> 字符串 */
static int s_itoa(int v, char *out) {
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char tmp[12]; int tl = 0;
    while (v > 0) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
    int p = 0;
    if (neg) out[p++] = '-';
    while (tl > 0) out[p++] = tmp[--tl];
    out[p] = 0;
    return p;
}

/* MAC 地址格式化: "XX:XX:XX:XX:XX:XX" */
static void fmt_mac(const u8 mac[6], char *out) {
    static const char h[] = "0123456789ABCDEF";
    int p = 0;
    for (int i = 0; i < 6; i++) {
        if (i) out[p++] = ':';
        out[p++] = h[(mac[i] >> 4) & 0xF];
        out[p++] = h[mac[i] & 0xF];
    }
    out[p] = 0;
}

/* 安全类型名称 */
static const char *sec_name(u8 sec) {
    switch (sec) {
    case NM_SEC_OPEN:  return "Open";
    case NM_SEC_WEP:   return "WEP";
    case NM_SEC_WPA:   return "WPA";
    case NM_SEC_WPA2:  return "WPA2";
    case NM_SEC_WPA3:  return "WPA3";
    default:           return "?";
    }
}

/* ---- INI 解析（填充字段值） ---- */
static void ini_fill_fields(const char *text, sfield *fields, int count) {
    char cur_sec[32]; cur_sec[0] = 0;
    const char *p = text;
    while (*p) {
        /* 取一行 */
        const char *ls = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - ls);
        if (*p == '\n') p++;
        /* 去尾部 \r */
        while (len > 0 && ls[len-1] == '\r') len--;
        /* 跳过前导空白 */
        int i = 0;
        while (i < len && (ls[i] == ' ' || ls[i] == '\t')) i++;
        if (i >= len) continue;
        if (ls[i] == '#' || ls[i] == ';') continue;
        if (ls[i] == '[') {
            int si = 0;
            i++;
            while (i < len && ls[i] != ']' && si < 31) cur_sec[si++] = ls[i++];
            cur_sec[si] = 0;
            continue;
        }
        /* key=value */
        int eq = -1;
        for (int j = i; j < len; j++) if (ls[j] == '=') { eq = j; break; }
        if (eq < 0) continue;
        int klen = eq - i;
        /* key 去尾部空白 */
        while (klen > 0 && (ls[i+klen-1] == ' ' || ls[i+klen-1] == '\t')) klen--;
        int vs = eq + 1;
        while (vs < len && (ls[vs] == ' ' || ls[vs] == '\t')) vs++;
        int vlen = len - vs;
        for (int f = 0; f < count; f++) {
            int skl = s_strlen(fields[f].section);
            int fkl = s_strlen(fields[f].key);
            if (skl != s_strlen(cur_sec) || !s_streqn(cur_sec, fields[f].section, skl)) continue;
            if (fkl != klen || !s_streqn(ls + i, fields[f].key, fkl)) continue;
            int n = vlen < 47 ? vlen : 47;
            for (int k = 0; k < n; k++) fields[f].value[k] = ls[vs + k];
            fields[f].value[n] = 0;
            fields[f].dirty = 0;
        }
    }
}

/* ---- FUCK 序列化（行保留：替换已暴露键的值，缺失键追加到末尾） ---- */
static u32 fuck_serialize(void) {
    char *out = g_fuck_out;
    u32 olen = 0;
    u8 appended[KFIELD_COUNT];
    for (int i = 0; i < (int)KFIELD_COUNT; i++) appended[i] = 0;

    char cur_sec[32]; cur_sec[0] = 0;
    const char *p = g_fuck_text;
    const char *end = g_fuck_text + g_fuck_len;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        int len = (int)(p - ls);
        if (p < end && *p == '\n') p++;
        int wlen = len;  /* 去 \r 用于解析 */
        while (wlen > 0 && ls[wlen-1] == '\r') wlen--;

        int replaced = 0;
        int i = 0;
        while (i < wlen && (ls[i] == ' ' || ls[i] == '\t')) i++;
        if (i < wlen && ls[i] == '[') {
            int si = 0; i++;
            while (i < wlen && ls[i] != ']' && si < 31) cur_sec[si++] = ls[i++];
            cur_sec[si] = 0;
        } else if (i < wlen && ls[i] != '#' && ls[i] != ';') {
            int eq = -1;
            for (int j = i; j < wlen; j++) if (ls[j] == '=') { eq = j; break; }
            if (eq >= 0) {
                int klen = eq - i;
                while (klen > 0 && (ls[i+klen-1] == ' ' || ls[i+klen-1] == '\t')) klen--;
                for (int f = 0; f < (int)KFIELD_COUNT; f++) {
                    int skl = s_strlen(g_kfields[f].section);
                    int fkl = s_strlen(g_kfields[f].key);
                    if (skl != s_strlen(cur_sec) || !s_streqn(cur_sec, g_kfields[f].section, skl)) continue;
                    if (fkl != klen || !s_streqn(ls + i, g_kfields[f].key, fkl)) continue;
                    /* 替换为 key=value */
                    int vl = s_strlen(g_kfields[f].value);
                    if (olen + (u32)fkl + 1 + (u32)vl + 1 < sizeof(g_fuck_out)) {
                        for (int k = 0; k < fkl; k++) out[olen++] = g_kfields[f].key[k];
                        out[olen++] = '=';
                        for (int k = 0; k < vl; k++) out[olen++] = g_kfields[f].value[k];
                        out[olen++] = '\n';
                    }
                    appended[f] = 1;
                    replaced = 1;
                    break;
                }
            }
        }
        if (!replaced) {
            /* 原样保留（含换行） */
            int raw = (int)(p - ls);
            if (olen + (u32)raw < sizeof(g_fuck_out)) {
                for (int k = 0; k < raw; k++) out[olen++] = ls[k];
            }
        }
    }
    /* 未在原文出现的 dirty 字段 -> 末尾追加（ini_get 首匹配生效，缺失键无冲突） */
    for (int f = 0; f < (int)KFIELD_COUNT; f++) {
        if (appended[f] || !g_kfields[f].dirty) continue;
        out[olen++] = '\n';
        out[olen++] = '[';
        for (const char *s = g_kfields[f].section; *s; ) out[olen++] = *s++;
        out[olen++] = ']'; out[olen++] = '\n';
        for (const char *s = g_kfields[f].key; *s; ) out[olen++] = *s++;
        out[olen++] = '=';
        for (const char *s = g_kfields[f].value; *s; ) out[olen++] = *s++;
        out[olen++] = '\n';
    }
    return olen;
}

/* ---- NETCONF 序列化（全量重写，格式与 netman 解析器一致） ---- */
static u32 conf_serialize(void) {
    static const char *order[] = { "mode", "device", "name", "password", "ip", "gateway", "dns" };
    u32 olen = 0;
    char *out = g_conf_out;
    for (int o = 0; o < 7; o++) {
        const char *k = order[o];
        const char *v = "";
        for (int f = 0; f < (int)NFIELD_COUNT; f++)
            if (s_streq(g_nfields[f].key, k)) { v = g_nfields[f].value; break; }
        const char *pfx = "network.";
        for (const char *s = pfx; *s; ) out[olen++] = *s++;
        for (const char *s = k; *s; ) out[olen++] = *s++;
        out[olen++] = '=';
        for (const char *s = v; *s; ) out[olen++] = *s++;
        out[olen++] = '\n';
    }
    return olen;
}

/* ---- 磁盘加载 / 保存 ---- */
static void load_configs(void) {
    u8 *data = 0; u32 size = 0;
    g_fuck_loaded = 0;
    if (f32_read_path_lfn("system/deshab64/FUCK", &data, &size) == 0 ||
        f32_read_path_lfn("FUCK", &data, &size) == 0) {
        if (size >= sizeof(g_fuck_text)) size = sizeof(g_fuck_text) - 1;
        for (u32 i = 0; i < size; i++) g_fuck_text[i] = (char)data[i];
        g_fuck_text[size] = 0;
        g_fuck_len = size;
        g_fuck_loaded = 1;
        ini_fill_fields(g_fuck_text, g_kfields, (int)KFIELD_COUNT);
    } else {
        g_fuck_len = 0;
        g_fuck_text[0] = 0;
    }

    g_net_loaded = 0;
    if (f32_read_root_file("NETCONF CNF", &data, &size) == 0) {
        if (size >= sizeof(g_conf_text)) size = sizeof(g_conf_text) - 1;
        for (u32 i = 0; i < size; i++) g_conf_text[i] = (char)data[i];
        g_conf_text[size] = 0;
        g_net_loaded = 1;
        ini_fill_fields(g_conf_text, g_nfields, (int)NFIELD_COUNT);
    } else {
        g_conf_text[0] = 0;
    }

    if (g_fuck_loaded && g_net_loaded)
        set_status("Loaded FUCK + NETCONF.CNF", DA_SUCCESS);
    else if (g_fuck_loaded)
        set_status("Loaded FUCK (NETCONF.CNF missing, defaults shown)", DA_WARNING);
    else
        set_status("Config files not found, defaults shown", DA_WARNING);
}

static void save_configs(void) {
    int ok_k = 0, ok_n = 0;
    if (g_page == 0) {
        u32 olen = fuck_serialize();
        if (f32_write_path_lfn("system/deshab64/FUCK", (const u8 *)g_fuck_out, olen) == 0 ||
            f32_write_path_lfn("FUCK", (const u8 *)g_fuck_out, olen) == 0) {
            ok_k = 1;
            for (int f = 0; f < (int)KFIELD_COUNT; f++) g_kfields[f].dirty = 0;
            /* 以新内容作为后续编辑基准 */
            if (olen < sizeof(g_fuck_text)) {
                for (u32 i = 0; i < olen; i++) g_fuck_text[i] = g_fuck_out[i];
                g_fuck_len = olen;
                g_fuck_text[olen] = 0;
                g_fuck_loaded = 1;
            }
        }
        set_status(ok_k ? "FUCK saved (reboot to apply)" : "FUCK save FAILED",
                   ok_k ? DA_SUCCESS : DA_ERROR);
    } else {
        /* page 1 (NETWORK) 和 page 2 (DEVICES) 都保存 NETCONF.CNF */
        u32 olen = conf_serialize();
        if (f32_write_root_file("NETCONF CNF", (const u8 *)g_conf_out, olen) == 0) {
            ok_n = 1;
            for (int f = 0; f < (int)NFIELD_COUNT; f++) g_nfields[f].dirty = 0;
        }
        set_status(ok_n ? "NETCONF.CNF saved (reboot to apply)" : "NETCONF save FAILED",
                   ok_n ? DA_SUCCESS : DA_ERROR);
    }
}

/* ---- 字段操作 ---- */
static sfield *cur_fields(void) { return g_page == 0 ? g_kfields : g_nfields; }
static int cur_count(void) {
    if (g_page == 2) {
        /* 设备页: 设备数 + 扫描结果数 + 2 按钮 */
        return g_dev_count + g_scan_count + 2;
    }
    return g_page == 0 ? (int)KFIELD_COUNT : (int)NFIELD_COUNT;
}

static void field_cycle(sfield *f, int dir) {
    if (f->type == FT_BOOL) {
        f->value[0] = (f->value[0] == '1') ? '0' : '1';
        f->value[1] = 0;
    } else if (f->type == FT_INT) {
        int v = 0;
        for (const char *s = f->value; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
        v += dir;
        if (v > f->max) v = f->min;
        if (v < f->min) v = f->max;
        int n = 0;
        if (v == 0) f->value[n++] = '0';
        char tmp[12]; int tl = 0;
        while (v > 0) { tmp[tl++] = (char)('0' + v % 10); v /= 10; }
        while (tl > 0) f->value[n++] = tmp[--tl];
        f->value[n] = 0;
    } else if (f->type == FT_CHOICE) {
        /* 在 "a|b|c" 中找当前项，切到下一项/上一项 */
        char opts[8][16]; int oc = 0;
        const char *s = f->choices;
        while (*s && oc < 8) {
            int l = 0;
            while (*s && *s != '|' && l < 15) opts[oc][l++] = *s++;
            opts[oc][l] = 0; oc++;
            if (*s == '|') s++;
        }
        int cur = -1;
        for (int i = 0; i < oc; i++) if (s_streq(f->value, opts[i])) { cur = i; break; }
        cur += dir;
        if (cur >= oc) cur = 0;
        if (cur < 0) cur = oc - 1;
        if (oc > 0) s_strcpy(f->value, opts[cur < 0 ? 0 : cur], sizeof(f->value));
    }
    f->dirty = 1;
}

/* ---- Net API 初始化 ---- */
static void init_net_api(const da_boot_context *ctx) {
    g_net_device_count = 0;
    g_net_device_info  = 0;
    g_net_scan_start   = 0;
    g_net_scan_count   = 0;
    g_net_scan_result  = 0;
    g_net_is_wireless  = 0;
    g_net_avail = 0;

    u64 api = ctx->dkm_kernel_api;
    if (!api) return;
    u64 net_api = *(u64 *)(api + 0x48);
    if (!net_api) return;

    g_net_device_count = (net_device_count_fn)*(u64 *)(net_api + 8);
    g_net_device_info  = (net_device_info_fn)*(u64 *)(net_api + 16);
    g_net_scan_start   = (net_scan_start_fn)*(u64 *)(net_api + 40);
    g_net_scan_count   = (net_scan_count_fn)*(u64 *)(net_api + 48);
    g_net_scan_result  = (net_scan_result_fn)*(u64 *)(net_api + 56);
    g_net_is_wireless  = (net_is_wireless_fn)*(u64 *)(net_api + 64);
    g_net_avail = 1;
}

/* ---- 设备枚举 ---- */
static void query_devices(void) {
    g_dev_count = 0;
    if (!g_net_device_count || !g_net_device_info) return;
    u32 count = g_net_device_count();
    if (count > DEV_MAX) count = DEV_MAX;
    for (u32 i = 0; i < count; i++) {
        nm_device_info info;
        da_memset(&info, 0, sizeof(info));
        if (g_net_device_info(i, &info) != 0) continue;
        /* 复制到本地 */
        g_devs[g_dev_count] = info;
        g_dev_count++;
    }
}

/* ---- WiFi 扫描 ---- */
static void trigger_scan(void) {
    if (!g_net_avail || !g_net_is_wireless || !g_net_scan_start) {
        set_status("Net API or WiFi scan not available", DA_ERROR);
        return;
    }
    if (g_sel < 0 || g_sel >= g_dev_count) {
        /* 如果当前不在设备行，尝试用上次选中的设备 */
        if (g_dev_count == 0) { set_status("No devices", DA_ERROR); return; }
    }
    /* 找到选中的无线设备 */
    int dev_idx = g_sel;
    if (dev_idx >= g_dev_count) dev_idx = 0;
    int wl = g_net_is_wireless(dev_idx);
    if (!wl) {
        /* 搜索第一个无线设备 */
        int found = -1;
        for (int i = 0; i < g_dev_count; i++) {
            if (g_net_is_wireless(i)) { found = i; break; }
        }
        if (found < 0) { set_status("No wireless device found", DA_WARNING); return; }
        dev_idx = found;
    }

    set_status("Scanning WiFi...", DA_ACCENT_LIGHT);
    int rc = g_net_scan_start(dev_idx);
    if (rc != 0) {
        set_status("WiFi scan failed", DA_ERROR);
        return;
    }

    g_scan_count = 0;
    if (g_net_scan_count) {
        int sc = g_net_scan_count(dev_idx);
        if (sc > SCAN_MAX) sc = SCAN_MAX;
        for (int i = 0; i < sc; i++) {
            if (g_net_scan_result(dev_idx, i, &g_scans[g_scan_count]) == 0)
                g_scan_count++;
        }
    }
    g_scan_dev_idx = dev_idx;

    char msg[64];
    s_strcpy(msg, "Scan found ", sizeof(msg));
    char nb[12]; s_itoa(g_scan_count, nb);
    int ml = s_strlen(msg);
    for (int i = 0; nb[i] && ml < 60; i++) msg[ml++] = nb[i];
    const char *suf = " networks";
    for (const char *s = suf; *s && ml < 62; ) msg[ml++] = *s++;
    msg[ml] = 0;
    set_status(msg, g_scan_count > 0 ? DA_SUCCESS : DA_WARNING);
}

/* ---- 设备选择（写入 NETCONF.CNF device 字段） ---- */
static void select_device(int idx) {
    if (idx < 0 || idx >= g_dev_count) return;
    if (!g_devs[idx].name) return;
    s_strcpy(g_nfields[NF_DEVICE].value, g_devs[idx].name, sizeof(g_nfields[NF_DEVICE].value));
    g_nfields[NF_DEVICE].dirty = 1;
    char msg[80];
    s_strcpy(msg, "Device set to ", sizeof(msg));
    int ml = s_strlen(msg);
    const char *nm = g_devs[idx].name;
    for (const char *s = nm; *s && ml < 78; ) msg[ml++] = *s++;
    const char *suf = " (F5 to save)";
    for (const char *s = suf; *s && ml < 79; ) msg[ml++] = *s++;
    msg[ml] = 0;
    set_status(msg, DA_SUCCESS);
}

/* ---- SSID 选择（写入 NETCONF.CNF name 字段） ---- */
static void select_ssid(int idx) {
    if (idx < 0 || idx >= g_scan_count) return;
    s_strcpy(g_nfields[NF_NAME].value, g_scans[idx].ssid, sizeof(g_nfields[NF_NAME].value));
    g_nfields[NF_NAME].dirty = 1;
    char msg[80];
    s_strcpy(msg, "SSID set to ", sizeof(msg));
    int ml = s_strlen(msg);
    for (const char *s = g_scans[idx].ssid; *s && ml < 78; ) msg[ml++] = *s++;
    const char *suf = " (F5 to save)";
    for (const char *s = suf; *s && ml < 79; ) msg[ml++] = *s++;
    msg[ml] = 0;
    set_status(msg, DA_SUCCESS);
}

/* ---- 布局 ---- */
#define TAB_Y      (DA_TITLEBAR_H + 4)
#define TAB_H      26
#define TAB_W      130
#define FIELD_Y0   (TAB_Y + TAB_H + 10)
#define ROW_H      24
#define LABEL_X    20
#define VALUE_X    320
#define VALUE_W    300
#define VALUE_H    20
#define BTN_Y_OFF  44            /* 底部按钮距状态栏高度 */
#define BTN_W      110
#define BTN_H      26

static int visible_rows(void) {
    int h = (int)g_ac.fb_h - FIELD_Y0 - BTN_Y_OFF - DA_STATUSBAR_H - 8;
    int r = h / ROW_H;
    return r < 1 ? 1 : r;
}

static void ensure_visible(void) {
    int vr = visible_rows();
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + vr) g_scroll = g_sel - vr + 1;
}

/* ---- 绘制 ---- */
static void draw_tab(int idx, const char *label, int x) {
    int active = (g_page == idx);
    u32 bg = active ? DA_BG_TERTIARY : DA_BG_SECONDARY;
    u32 fg = active ? DA_ACCENT_LIGHT : DA_TEXT_DIM;
    da_fill_rounded_rect(&g_ac, x, TAB_Y, TAB_W - 8, TAB_H, bg, 6);
    da_rect_outline(&g_ac, x, TAB_Y, TAB_W - 8, TAB_H, active ? DA_BORDER_FOCUS : DA_BORDER, 6);
    int lx = x + 14;
    da_draw_string(&g_ac, label, lx, TAB_Y + 4, fg, bg, 12);
}

static void draw_field_row(int fi, int row_y) {
    sfield *f = &cur_fields()[fi];
    int selected = (fi == g_sel);
    i64 fw = (i64)g_ac.fb_w - 40;
    if (selected)
        da_fill_rect(&g_ac, LABEL_X - 4, row_y - 2, fw, ROW_H, DA_BG_SECONDARY);

    /* 标签: section/key */
    char label[64];
    int lp = 0;
    for (const char *s = f->section; *s && lp < 60; ) label[lp++] = *s++;
    label[lp++] = '/';
    for (const char *s = f->key; *s && lp < 62; ) label[lp++] = *s++;
    label[lp] = 0;
    da_draw_string(&g_ac, label, LABEL_X, row_y + 1, selected ? DA_TEXT_PRIMARY : DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 值框 */
    u32 vbg = selected ? DA_BG_TERTIARY : DA_BG_SECONDARY;
    u32 vborder = selected ? DA_BORDER_FOCUS : DA_BORDER;
    da_fill_rect(&g_ac, VALUE_X, row_y, VALUE_W, VALUE_H, vbg);
    da_rect_outline(&g_ac, VALUE_X, row_y, VALUE_W, VALUE_H, vborder, 2);

    char disp[56];
    int dp = 0;
    if (f->type == FT_BOOL || f->type == FT_CHOICE || f->type == FT_INT) {
        disp[dp++] = '<'; disp[dp++] = ' ';
        for (const char *s = f->value; *s && dp < 52; ) disp[dp++] = *s++;
        disp[dp++] = ' '; disp[dp++] = '>';
    } else {
        for (const char *s = f->value; *s && dp < 52; ) disp[dp++] = *s++;
    }
    disp[dp] = 0;
    u32 vfg;
    if (f->type == FT_BOOL)
        vfg = (f->value[0] == '1') ? DA_SUCCESS : DA_ERROR;
    else
        vfg = DA_ACCENT_LIGHT;
    da_draw_string(&g_ac, disp, VALUE_X + 8, row_y + 1, vfg, vbg, 12);

    /* 文本字段编辑光标 */
    if (selected && f->type == FT_TEXT) {
        int vl = s_strlen(f->value);
        i64 cx = VALUE_X + 8 + (i64)vl * 12;
        if (cx < VALUE_X + VALUE_W - 12)
            da_fill_rect(&g_ac, cx, row_y + VALUE_H - 4, 11, 2, DA_ACCENT);
    }
    /* dirty 标记 */
    if (f->dirty)
        da_draw_char(&g_ac, '*', VALUE_X + VALUE_W + 6, row_y + 1, DA_WARNING,
                     selected ? DA_BG_SECONDARY : DA_BG_PRIMARY);
}

/* ---- 设备页绘制 ---- */
static int dev_item_count(void) {
    return g_dev_count + g_scan_count + 2;
}

/* 返回设备页第 sel 项的类型和索引 */
static void dev_item_info(int sel, int *type, int *idx) {
    if (sel < g_dev_count) { *type = 0; *idx = sel; return; }       /* 设备 */
    int scan_base = g_dev_count + 1;  /* +1 跳过分隔线 */
    if (sel >= scan_base && sel < scan_base + g_scan_count) { *type = 1; *idx = sel - scan_base; return; } /* 扫描 */
    int btn_base = scan_base + g_scan_count;
    if (sel == btn_base) { *type = 2; *idx = 0; return; }   /* SCAN */
    if (sel == btn_base + 1) { *type = 3; *idx = 0; return; } /* SELECT */
    *type = -1; *idx = -1;
}

static void draw_device_row(int idx, int row_y, int selected) {
    nm_device_info *d = &g_devs[idx];
    if (selected)
        da_fill_rect(&g_ac, LABEL_X - 4, row_y - 2, (i64)g_ac.fb_w - 40, ROW_H, DA_BG_SECONDARY);

    /* 索引 */
    char idx_s[8]; s_itoa(idx, idx_s);
    da_draw_string(&g_ac, "[", LABEL_X, row_y + 1,
                   selected ? DA_TEXT_PRIMARY : DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    da_draw_string(&g_ac, idx_s, LABEL_X + 12, row_y + 1,
                   selected ? DA_TEXT_PRIMARY : DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    da_draw_string(&g_ac, "]", LABEL_X + 24, row_y + 1,
                   selected ? DA_TEXT_PRIMARY : DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 名称 */
    int nx = LABEL_X + 40;
    const char *nm = d->name ? d->name : "<noname>";
    u32 name_fg = DA_ACCENT_LIGHT;
    if (d->flags & NM_NET_F_WIRELESS) name_fg = DA_WARNING;
    da_draw_string(&g_ac, nm, nx, row_y + 1, name_fg,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* MAC */
    char mac_s[20]; fmt_mac(d->mac, mac_s);
    int mac_x = LABEL_X + 200;
    da_draw_string(&g_ac, mac_s, mac_x, row_y + 1, DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 状态标志 */
    int sx = mac_x + 180;
    if (d->flags & NM_NET_F_LINK_UP)
        da_draw_string(&g_ac, "LINK", sx, row_y + 1, DA_SUCCESS,
                       selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    else
        da_draw_string(&g_ac, "down", sx, row_y + 1, DA_ERROR,
                       selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    sx += 60;
    if (d->flags & NM_NET_F_TX_READY)
        da_draw_string(&g_ac, "TX", sx, row_y + 1, DA_SUCCESS,
                       selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    sx += 30;
    if (d->flags & NM_NET_F_RX_READY)
        da_draw_string(&g_ac, "RX", sx, row_y + 1, DA_SUCCESS,
                       selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    sx += 30;
    if (d->flags & NM_NET_F_WIRELESS)
        da_draw_string(&g_ac, "WiFi", sx, row_y + 1, DA_WARNING,
                       selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 当前选中设备标记 */
    if (d->name && s_streq(g_nfields[NF_DEVICE].value, d->name))
        da_draw_char(&g_ac, '*', (i64)g_ac.fb_w - 30, row_y + 1, DA_SUCCESS,
                     selected ? DA_BG_SECONDARY : DA_BG_PRIMARY);
}

static void draw_scan_row(int idx, int row_y, int selected) {
    nm_scan_result *r = &g_scans[idx];
    if (selected)
        da_fill_rect(&g_ac, LABEL_X - 4, row_y - 2, (i64)g_ac.fb_w - 40, ROW_H, DA_BG_SECONDARY);

    /* SSID */
    const char *ssid = r->ssid[0] ? r->ssid : "<hidden>";
    da_draw_string(&g_ac, ssid, LABEL_X, row_y + 1,
                   selected ? DA_TEXT_PRIMARY : DA_ACCENT_LIGHT,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* RSSI */
    char rssi_s[12];
    rssi_s[0] = 0;
    s_itoa(r->rssi, rssi_s);
    int rx = LABEL_X + 260;
    da_draw_string(&g_ac, rssi_s, rx, row_y + 1,
                   r->rssi > -50 ? DA_SUCCESS : (r->rssi > -70 ? DA_WARNING : DA_ERROR),
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    da_draw_string(&g_ac, "dBm", rx + 40, row_y + 1, DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 信道 */
    char ch_s[8]; s_itoa(r->channel, ch_s);
    da_draw_string(&g_ac, "ch", rx + 90, row_y + 1, DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);
    da_draw_string(&g_ac, ch_s, rx + 110, row_y + 1, DA_TEXT_DIM,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 加密 */
    da_draw_string(&g_ac, sec_name(r->security), rx + 140, row_y + 1,
                   r->security == NM_SEC_OPEN ? DA_SUCCESS : DA_WARNING,
                   selected ? DA_BG_SECONDARY : DA_BG_PRIMARY, 12);

    /* 当前选中 SSID 标记 */
    if (r->ssid[0] && s_streq(g_nfields[NF_NAME].value, r->ssid))
        da_draw_char(&g_ac, '*', (i64)g_ac.fb_w - 30, row_y + 1, DA_SUCCESS,
                     selected ? DA_BG_SECONDARY : DA_BG_PRIMARY);
}

static void draw_device_button(int x, int y, const char *label, int selected) {
    u32 bg = selected ? DA_BG_TERTIARY : DA_BG_SECONDARY;
    u32 border = selected ? DA_BORDER_FOCUS : DA_BORDER;
    da_fill_rounded_rect(&g_ac, x, y, BTN_W, BTN_H, bg, 6);
    da_rect_outline(&g_ac, x, y, BTN_W, BTN_H, border, 6);
    int ll = s_strlen(label);
    da_draw_string(&g_ac, label, x + (BTN_W - ll * 12) / 2, y + 4,
                   DA_ACCENT_LIGHT, bg, 12);
}

static void draw_device_page(void) {
    int vr = visible_rows();
    int count = dev_item_count();

    /* 列表 */
    for (int r = 0; r < vr; r++) {
        int item = g_scroll + r;
        if (item >= count) break;
        int row_y = FIELD_Y0 + r * ROW_H;

        int type, idx;
        dev_item_info(item, &type, &idx);

        if (type == 0) {
            draw_device_row(idx, row_y, item == g_sel);
        } else if (type == 1) {
            draw_scan_row(idx, row_y, item == g_sel);
        } else if (type == 2) {
            draw_device_button(LABEL_X, row_y, "SCAN (S)", item == g_sel);
        } else if (type == 3) {
            draw_device_button(LABEL_X + BTN_W + 12, row_y, "SELECT", item == g_sel);
        }
    }

    /* 分隔线: 设备列表和扫描结果之间 */
    int sep_item = g_dev_count;  /* 分隔线位置 */
    int sep_row = sep_item - g_scroll;
    if (sep_row >= 0 && sep_row < vr) {
        int sy = FIELD_Y0 + sep_row * ROW_H + ROW_H / 2;
        da_fill_rect(&g_ac, LABEL_X, sy, (i64)g_ac.fb_w - 40, 1, DA_BORDER);
        const char *hdr = (g_scan_count > 0) ? "--- WiFi Scan Results ---" : "--- WiFi Scan (press S) ---";
        int hx = LABEL_X + 20;
        da_draw_string(&g_ac, hdr, hx, sy - 8, DA_TEXT_DIM, DA_BG_PRIMARY, 12);
    }

    /* 滚动指示 */
    if (count > vr) {
        char info[32];
        int ip = 0;
        int a = g_scroll + 1, b = g_scroll + vr > count ? count : g_scroll + vr;
        char nb[12]; int nl;
        nl = 0; while (a) { nb[nl++] = (char)('0' + a % 10); a /= 10; }
        while (nl > 0) info[ip++] = nb[--nl];
        info[ip++] = '-';
        nl = 0; if (b == 0) nb[nl++] = '0'; while (b) { nb[nl++] = (char)('0' + b % 10); b /= 10; }
        while (nl > 0) info[ip++] = nb[--nl];
        info[ip++] = '/';
        nl = 0; { int c2 = count; if (c2 == 0) nb[nl++] = '0'; while (c2) { nb[nl++] = (char)('0' + c2 % 10); c2 /= 10; } }
        while (nl > 0) info[ip++] = nb[--nl];
        info[ip] = 0;
        da_draw_string(&g_ac, info, (i64)g_ac.fb_w - 120, TAB_Y + 6, DA_TEXT_DIM, DA_BG_PRIMARY, 12);
    }
}

static void draw_button(int x, int y, const char *label, int hot) {
    u32 bg = hot ? DA_BG_TERTIARY : DA_BG_SECONDARY;
    da_fill_rounded_rect(&g_ac, x, y, BTN_W, BTN_H, bg, 6);
    da_rect_outline(&g_ac, x, y, BTN_W, BTN_H, DA_BORDER_FOCUS, 6);
    int ll = s_strlen(label);
    da_draw_string(&g_ac, label, x + (BTN_W - ll * 12) / 2, y + 4, DA_ACCENT_LIGHT, bg, 12);
}

static void redraw_all(void) {
    da_cursor_restore(&g_ac, &g_cursor);
    da_fill_bg(&g_ac, DA_BG_PRIMARY);
    da_draw_titlebar(&g_ac, "Settings", (i64)g_ac.fb_w);

    draw_tab(0, "KERNEL", 16);
    draw_tab(1, "NETWORK", 16 + TAB_W);
    draw_tab(2, "DEVICES", 16 + 2 * TAB_W);

    if (g_page < 2) {
        /* 字段列表 */
        int vr = visible_rows();
        int count = cur_count();
        for (int r = 0; r < vr; r++) {
            int fi = g_scroll + r;
            if (fi >= count) break;
            draw_field_row(fi, FIELD_Y0 + r * ROW_H);
        }
        /* 滚动指示 */
        if (count > vr) {
            char info[32];
            int ip = 0;
            int a = g_scroll + 1, b = g_scroll + vr > count ? count : g_scroll + vr;
            char nb[12]; int nl;
            nl = 0; while (a) { nb[nl++] = (char)('0' + a % 10); a /= 10; }
            while (nl > 0) info[ip++] = nb[--nl];
            info[ip++] = '-';
            nl = 0; if (b == 0) nb[nl++] = '0'; while (b) { nb[nl++] = (char)('0' + b % 10); b /= 10; }
            while (nl > 0) info[ip++] = nb[--nl];
            info[ip++] = '/';
            nl = 0; { int c2 = count; if (c2 == 0) nb[nl++] = '0'; while (c2) { nb[nl++] = (char)('0' + c2 % 10); c2 /= 10; } }
            while (nl > 0) info[ip++] = nb[--nl];
            info[ip] = 0;
            da_draw_string(&g_ac, info, (i64)g_ac.fb_w - 120, TAB_Y + 6, DA_TEXT_DIM, DA_BG_PRIMARY, 12);
        }
    } else {
        /* 设备页 */
        if (!g_net_avail) {
            da_draw_string(&g_ac, "Net API not available (no kernel network support)",
                           LABEL_X, FIELD_Y0, DA_WARNING, DA_BG_PRIMARY, 12);
        } else if (g_dev_count == 0) {
            da_draw_string(&g_ac, "No network devices found",
                           LABEL_X, FIELD_Y0, DA_WARNING, DA_BG_PRIMARY, 12);
        } else {
            draw_device_page();
        }
    }

    /* 底部按钮 + 状态消息 */
    int by = (int)g_ac.fb_h - DA_STATUSBAR_H - BTN_Y_OFF + 8;
    draw_button(20, by, "SAVE (F5)", 0);
    draw_button(20 + BTN_W + 12, by, "RELOAD", 0);
    if (g_status[0])
        da_draw_string(&g_ac, g_status, 20 + 2 * (BTN_W + 12) + 8, by + 4,
                       g_status_color, DA_BG_PRIMARY, 12);

    /* 状态栏 */
    const char *hint;
    if (g_page == 0)
        hint = "F1 Kernel | F2 Network | F3 Devices | F5 Save | F6 Reload | Esc Quit | reboot to apply";
    else if (g_page == 1)
        hint = "F1 Kernel | F2 Network | F3 Devices | F5 Save | F6 Reload | Esc Quit | netman reads at boot";
    else
        hint = "F1 Kernel | F2 Network | F3 Devices | S Scan | Enter Select | F5 Save | Esc Quit";
    da_draw_statusbar(&g_ac, hint, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    da_cursor_save(&g_ac, &g_cursor);
    da_cursor_draw(&g_ac, &g_cursor, DA_CURSOR_COLOR);
}

/* ---- 页面切换 ---- */
static void switch_page(int p) {
    if (p == g_page) return;
    g_sel_saved[g_page] = g_sel;
    g_scroll_saved[g_page] = g_scroll;
    g_page = p;
    g_sel = g_sel_saved[p];
    g_scroll = g_scroll_saved[p];
    ensure_visible();
}

/* ---- 鼠标点击 ---- */
static void handle_click(int mx, int my) {
    /* tab 区 */
    if (my >= TAB_Y && my < TAB_Y + TAB_H) {
        if (mx >= 16 && mx < 16 + TAB_W - 8) { switch_page(0); return; }
        if (mx >= 16 + TAB_W && mx < 16 + 2 * TAB_W - 8) { switch_page(1); return; }
        if (mx >= 16 + 2 * TAB_W && mx < 16 + 3 * TAB_W - 8) { switch_page(2); return; }
    }

    if (g_page < 2) {
        /* 字段区 */
        if (my >= FIELD_Y0) {
            int row = (my - FIELD_Y0) / ROW_H;
            int fi = g_scroll + row;
            if (fi >= 0 && fi < cur_count() && row < visible_rows()) {
                g_sel = fi;
                sfield *f = &cur_fields()[fi];
                if (mx >= VALUE_X && mx < VALUE_X + VALUE_W && f->type != FT_TEXT)
                    field_cycle(f, 1);
                return;
            }
        }
    } else {
        /* 设备页 */
        if (my >= FIELD_Y0) {
            int row = (my - FIELD_Y0) / ROW_H;
            int item = g_scroll + row;
            if (item >= 0 && item < dev_item_count() && row < visible_rows()) {
                g_sel = item;
                int type, idx;
                dev_item_info(item, &type, &idx);
                if (type == 0) {
                    select_device(idx);
                } else if (type == 1) {
                    select_ssid(idx);
                } else if (type == 2) {
                    trigger_scan();
                } else if (type == 3) {
                    /* SELECT 按钮: 选择当前选中设备 */
                    if (g_sel < g_dev_count) select_device(g_sel);
                }
                return;
            }
        }
    }

    /* 按钮区 */
    int by = (int)g_ac.fb_h - DA_STATUSBAR_H - BTN_Y_OFF + 8;
    if (my >= by && my < by + BTN_H) {
        if (mx >= 20 && mx < 20 + BTN_W) { save_configs(); return; }
        if (mx >= 20 + BTN_W + 12 && mx < 20 + 2 * BTN_W + 12) {
            load_configs();
            if (g_page == 2) query_devices();
            return;
        }
    }
}

/* ---- 主入口 ---- */
__attribute__((visibility("default")))
void dsk_entry(const da_boot_context *ctx) {
    __asm__ volatile("cli");
    da_slog("settings", "boot");

    if (!ctx || ctx->magic != DA_BOOT_MAGIC) {
        da_slog("settings", "bad context");
        for (;;) __asm__("hlt");
    }

    da_init(&g_ac, ctx);
    if (!g_ac.block_read || !g_ac.block_write) {
        da_slog("settings", "block api missing");
        for (;;) __asm__("hlt");
    }
    f32_init((f32_block_read_fn)g_ac.block_read, (f32_block_write_fn)g_ac.block_write);

    /* 初始化 Net API */
    init_net_api(ctx);
    if (g_net_avail) query_devices();

    g_status[0] = 0;
    load_configs();

    da_mouse_init();
    g_mouse.idx = 0; g_mouse.has_pkt = 0;
    da_cursor_init(&g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h);

    int shift = 0, e0 = 0;
    int prev_btn = 0;
    redraw_all();

    for (;;) {
        /* 鼠标 */
        if (da_mouse_poll(&g_mouse, &g_cursor, (i64)g_ac.fb_w, (i64)g_ac.fb_h)) {
            if (g_cursor.btn && !prev_btn)
                handle_click(g_cursor.mx, g_cursor.my);
            prev_btn = g_cursor.btn;
            redraw_all();
            continue;
        }
        /* 键盘 */
        u8 st = inb(0x64);
        if (!(st & 1)) { __asm__("pause"); continue; }
        if (st & 0x20) continue;   /* AUX 数据留给鼠标路径 */
        u8 sc = inb(0x60);
        if (sc == 0xE0) { e0 = 1; continue; }
        if (sc == 0x2A || sc == 0x36) { shift = 1; continue; }
        if (sc == 0xAA || sc == 0xB6) { shift = 0; continue; }
        if (sc & 0x80) { e0 = 0; continue; }

        if (sc == 0x01 && !e0) {   /* Esc */
            da_slog("settings", "exit");
            da_cursor_restore(&g_ac, &g_cursor);
            return;
        }
        if (!e0 && sc == 0x3B) { switch_page(0); redraw_all(); continue; }  /* F1 */
        if (!e0 && sc == 0x3C) { switch_page(1); redraw_all(); continue; }  /* F2 */
        if (!e0 && sc == 0x3D) { switch_page(2); redraw_all(); continue; }  /* F3 */
        if (!e0 && sc == 0x0F) { switch_page((g_page + 1) % 3); redraw_all(); continue; } /* Tab */
        if (!e0 && sc == 0x3F) { save_configs(); redraw_all(); continue; }  /* F5 */
        if (!e0 && sc == 0x40) { load_configs(); if (g_page == 2) query_devices(); redraw_all(); continue; } /* F6 */

        int count = cur_count();
        if (e0) {
            if (sc == 0x48) {           /* Up */
                if (g_sel > 0) g_sel--;
                ensure_visible();
            } else if (sc == 0x50) {    /* Down */
                if (g_sel < count - 1) g_sel++;
                ensure_visible();
            } else if (sc == 0x4B) {    /* Left */
                if (g_page < 2) {
                    sfield *f = &cur_fields()[g_sel];
                    if (f->type != FT_TEXT) field_cycle(f, -1);
                }
            } else if (sc == 0x4D) {    /* Right */
                if (g_page < 2) {
                    sfield *f = &cur_fields()[g_sel];
                    if (f->type != FT_TEXT) field_cycle(f, 1);
                }
            } else if (sc == 0x47) { g_sel = 0; ensure_visible(); }         /* Home */
            else if (sc == 0x4F) { g_sel = count - 1; ensure_visible(); }   /* End */
            else if (sc == 0x49) {      /* PgUp */
                g_sel -= visible_rows();
                if (g_sel < 0) g_sel = 0;
                ensure_visible();
            } else if (sc == 0x51) {    /* PgDn */
                g_sel += visible_rows();
                if (g_sel > count - 1) g_sel = count - 1;
                ensure_visible();
            }
            e0 = 0;
            redraw_all();
            continue;
        }

        /* 设备页特殊键 */
        if (g_page == 2) {
            if (sc == 0x1C || sc == 0x39) { /* Enter / Space */
                int type, idx;
                dev_item_info(g_sel, &type, &idx);
                if (type == 0) select_device(idx);
                else if (type == 1) select_ssid(idx);
                else if (type == 2) trigger_scan();
                else if (type == 3) {
                    if (g_sel < g_dev_count) select_device(g_sel);
                }
                redraw_all();
                continue;
            }
            /* 'S' 键扫描 */
            if (sc == 0x1F) {  /* S */
                trigger_scan();
                redraw_all();
                continue;
            }
            /* 其他键在设备页不处理 */
            continue;
        }

        /* 字段页键盘输入 */
        sfield *f = &cur_fields()[g_sel];
        if (sc == 0x1C || sc == 0x39) { /* Enter / Space */
            if (f->type != FT_TEXT) field_cycle(f, 1);
            redraw_all();
            continue;
        }
        if (sc == 0x0E) {               /* Backspace */
            if (f->type == FT_TEXT) {
                int l = s_strlen(f->value);
                if (l > 0) { f->value[l-1] = 0; f->dirty = 1; }
            }
            redraw_all();
            continue;
        }
        char c = da_scan_to_ascii(sc, shift);
        if (c && c >= 32 && c <= 126 && f->type == FT_TEXT) {
            int l = s_strlen(f->value);
            if (l < 46) {
                f->value[l] = c;
                f->value[l+1] = 0;
                f->dirty = 1;
            }
            redraw_all();
            continue;
        }
    }
}
