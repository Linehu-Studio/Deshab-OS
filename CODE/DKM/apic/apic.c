/* DKM APIC Driver — APIC discovery + IOAPIC init + LAPIC enable + 路由接管
 * Stage 0, required, depends on "timer", provides "irq" and "apic".
 *
 * 真机路径:
 *   1. LAPIC 使能 + Spurious Interrupt Vector 注册
 *   2. IOAPIC 重定向表初始化: 逐条目编程, 将 ISA IRQ 映射到 vector 0x20-0x2F
 *      (兼容 PIC 布局), 逐条目 mask
 *   3. MADT ISO (Interrupt Source Override) 处理: 按 ACPI 规范修正 IRQ→GSI 映射
 *   4. SIPI 多核启动框架 (预留, 未实现)
 *
 * B7 路由接管 (apic_route):
 *   FUCK [drivers] apic_route=1 时, 本驱动在初始化末尾尝试把中断路由
 *   从 8259 PIC 切换到 IOAPIC→LAPIC 路径:
 *     a. 经 kernel_api.register_apic_eoi 向 UTSM 注册 LAPIC EOI 钩子
 *        (B7 阶段2 接口; idt_handler 两个 IRQ 分支在分发末尾调用之)
 *     b. 注册 PIT tick 计数 handler 于 vector 0x20 — 即 idt_handler 的
 *        PIC 分支 (0x20-0x2F), 与键盘 0x21/鼠标 0x2C/网卡 0x2B 同一代码
 *        路径; handler 返回 0 由钩子完成 LAPIC EOI, 重复投递 >=2 即证明
 *        "legacy vector 分支 + 钩子 EOI" 全链路可持续 (阶段1 用 0xE0 自
 *        EOI 只证明了 APIC 分支)
 *     c. 按 ISO 表解析 PIT GSI (QEMU q35: IRQ0→GSI2), 解屏蔽该条目
 *     d. LAPIC TPR=0 全放行, PIC IMR 全 1 屏蔽 (QEMU 无功能 IMCR 寄存器,
 *        PIC 全屏蔽即等效切换; 8259 与 IOAPIC 同时监听 IRQ 线, 全屏蔽避免双投递)
 *     e. PIT ch0 mode3 100Hz 产生真实硬件中断, 500ms 窗口内计数 >=2 次
 *        即证明 IOAPIC→LAPIC→IDT→handler→钩子EOI 全链路
 *     f. 验证失败完整降级: IOAPIC 全 mask + 恢复原 PIC IMR + 注销 handler
 *        与 EOI 钩子, 系统行为与切换前完全一致
 *
 * B7 阶段2 (apic_route_legacy):
 *   EOI 钩子落地后, legacy IRQ1(键盘 0x21)/IRQ12(鼠标 0x2C)/IRQ11(e1000
 *   0x2B) 重定向项可持续投递, 不再是 one-shot。ISA 线按 MADT ISO 极性/触发
 *   (缺省 edge/active-high); PCI 线 (IRQ11) 无 ISO 覆盖时强制 level/
 *   active-low (PCI INTx 规范; QEMU 内部 pci irq 电平语义与之匹配,
 *   Linux/Windows 同配置)。e1000 handler 读 ICR 清源后电平 deassert,
 *   由钩子 EOI 完成 LAPIC 侧清扫。
 *
 * apic_diag=1 诊断:
 *   - HPET 绝对时基对拍: 解析 ACPI HPET 表 (QEMU q35 有, 100MHz 主计数器),
 *     用主计数器实测 tsc_calibrate 的 10ms 窗口与 route 验证的 500ms 窗口,
 *     定论 "tick 2x" 根因 (TSC 校准测半 vs PIT mode3 双触发)
 *   - PS/2 鼠标流: 初始化 AUX 端口 + 注册 IRQ12 诊断 handler, 配合
 *     QEMU monitor mouse_move 验证 IRQ12 持续投递 (计数 + 观察窗)
 *   diag 只增观测不改变量路由语义, 默认 0 关闭。
 *
 * QEMU 兼容:
 *   - QEMU 默认只提供 1 个 IOAPIC, 24 条 GSI
 *   - 实机可能有多个 IOAPIC, GSI 范围更大
 *   - 实机必须处理 MADT ISO (IRQ0→GSI2 等)
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
    const void *block;
    const void *mmio;
    void *(*mm_map_mmio)(u64 phys, u64 size);
    void (*mm_unmap_mmio)(void *virt, u64 size);
    /* B7 阶段2 尾部追加 (旧内核无此字段 -> 判空使用) */
    void (*register_apic_eoi)(void (*eoi_fn)(void));
    /* B7 阶段3 尾部追加 (本驱动不使用, 保持布局与 UTSM 一致) */
    int  (*irq_vector_alloc)(void);
    void (*irq_vector_free)(int vector);
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

static const char *const g_depends[] = { "timer" };
static const char *const g_provides[] = { "irq", "apic" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "apic",
    .version        = "0.2.0",
    .vendor         = "Deshab",
    .driver_class   = 3,   /* DKM_CLASS_INTERRUPT */
    .stage          = 0,
    .flags          = 1,   /* DKM_F_REQUIRED */
    .priority       = 0,
    .depends        = g_depends,
    .depends_count  = 1,
    .provides       = g_provides,
    .provides_count = 2,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

typedef struct __attribute__((packed)) {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr;
} acpi_rsdp_v1;

typedef struct __attribute__((packed)) {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr;
    u32 length;
    u64 xsdt_addr;
    u8 ext_checksum;
    u8 reserved[3];
} acpi_rsdp_v2;

typedef struct __attribute__((packed)) {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} acpi_sdt_header;

typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 lapic_addr;
    u32 flags;
} acpi_madt;

/* ACPI HPET 表 (signature "HPET") — apic_diag 绝对时基对拍用 */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 event_timer_block_id;
    /* ACPI Generic Address Structure (GAS) */
    u8  addr_space_id;       /* 0 = system memory */
    u8  register_bit_width;
    u8  register_bit_offset;
    u8  access_size;
    u64 base_addr;           /* HPET MMIO 物理基址 (QEMU: 0xFED00000) */
    u8  hpet_number;
    u16 min_clock_tick;
    u8  page_protection;
} acpi_hpet;

/* Limine boot module 最小类型 (与 bootfs.drv / UTSM limine.h 布局一致) */
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

/* ---- IOAPIC 信息存储（真机多 IOAPIC 支持） ---- */

#define MAX_IOAPIC 4
#define MAX_ISO    16
#define MAX_LAPIC  256

struct ioapic_info {
    u8  id;
    u32 gsi_base;
    u32 gsi_count;      /* 从 IOAPIC VER 寄存器读取: max_redir_entry + 1 */
    u64 mmio_phys;      /* IOAPIC MMIO 物理基址 */
    volatile u32 *mmio; /* HHDM/MMIO 窗口映射后的虚拟地址, 0=未映射 */
};

struct iso_info {
    u8  bus_source;      /* ISA bus source IRQ (0-15) */
    u32 gsi;             /* 全局系统中断号 */
    u16 flags;           /* 极性/触发模式标志 */
};

struct lapic_entry {
    u8  apic_id;
    u32 flags;           /* MADT LAPIC flags: bit0=enabled */
};

struct madt_info {
    u64 lapic_phys;
    u32 flags;
    u32 lapic_count;
    u32 ioapic_count;
    u32 iso_count;

    struct ioapic_info  ioapics[MAX_IOAPIC];
    struct iso_info     isos[MAX_ISO];
    struct lapic_entry  lapics[MAX_LAPIC];
};

/* ---- B7 路由接管配置 (FUCK [drivers]) ---- */
struct apic_config {
    int lapic_enable;      /* lapic_enable, 默认 1 */
    int ioapic_init;       /* ioapic_init, 默认 1 */
    int apic_route;        /* apic_route, 默认 0 (PIC 路由, 现状) */
    int apic_route_legacy; /* apic_route_legacy, 默认 0 */
    int apic_diag;         /* apic_diag, 默认 0 (HPET 校准/投递计数/鼠标流诊断) */
};

static const struct dkm_log_api *g_log;
static struct apic_config g_cfg = { 1, 1, 0, 0, 0 };

static volatile u32 *g_lapic;      /* LAPIC MMIO 映射基址, 0=未映射 */
static u32 g_lapic_id;             /* BSP Local APIC ID (IOAPIC 目的 ID) */
static volatile u32 g_tick_count;  /* PIT tick 计数 (路由验证) */
static int g_route_active;         /* 1=IOAPIC 路由已接管 */

static __inline__ void apic_mb(void) {
    __asm__ volatile("" ::: "memory");
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

/* 在 NUL 结尾字符串中查找子串 (cmdline 匹配用) */
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

static u8 checksum8(const u8 *data, u32 len) {
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void log_hex(const char *prefix, u64 val) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    u32 pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

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

/* ---- I/O 端口 (PIC / PIT) ---- */
static __inline__ void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static __inline__ void io_wait(void) {
    outb(0x80, 0);
}

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static u64 rdmsr(u32 msr) {
    u32 lo;
    u32 hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void wrmsr(u32 msr, u64 val) {
    u32 lo = (u32)(val & 0xffffffffu);
    u32 hi = (u32)(val >> 32);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

static int cpu_has_lapic(void) {
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    (void)a;
    (void)b;
    (void)c;
    return (d & (1u << 9)) != 0;
}

/* ---- FUCK 配置自解析 (DKM 侧, 不依赖 UTSM ini_parser) ---- *
 * UTSM 通过 Limine boot module (cmdline="fuck:config") 预加载 FUCK;
 * kernel_api.boot_modules_response 对所有 DKM 驱动可见。
 * 此处实现最小分区感知 INI 扫描: 只找 [drivers] 分区下的目标键。
 * 缺失文件/缺失键一律回退默认值, 行为与无配置时一致。
 */

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
    /* 十进制整数 (允许前导 '-') */
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

static void load_fuck_config(const struct dkm_kernel_api *api) {
    /* 编译期默认: 与 FUCK 注释承诺一致 (无配置时行为不变) */
    g_cfg.lapic_enable = 1;
    g_cfg.ioapic_init = 1;
    g_cfg.apic_route = 0;
    g_cfg.apic_route_legacy = 0;
    g_cfg.apic_diag = 0;

    const struct limine_module_response_min *resp =
        (const struct limine_module_response_min *)api->boot_modules_response;
    if (!resp || !resp->modules) {
        g_log->warn("[apic] no boot modules; FUCK defaults in effect");
        return;
    }

    for (u64 i = 0; i < resp->module_count; i++) {
        struct limine_file_min *f = resp->modules[i];
        if (!f || !f->cmdline || !f->address || !f->size) continue;
        if (!str_contains(f->cmdline, "fuck:config")) continue;

        const char *text = (const char *)f->address;
        u64 size = f->size;
        g_log->info("[apic] FUCK config module found");
        log_hex("[apic] FUCK size=", size);

        g_cfg.lapic_enable = fuck_get_int(text, size, "drivers", "lapic_enable", 1);
        g_cfg.ioapic_init = fuck_get_int(text, size, "drivers", "ioapic_init", 1);
        g_cfg.apic_route = fuck_get_int(text, size, "drivers", "apic_route", 0);
        g_cfg.apic_route_legacy = fuck_get_int(text, size, "drivers", "apic_route_legacy", 0);
        g_cfg.apic_diag = fuck_get_int(text, size, "drivers", "apic_diag", 0);

        log_dec("[apic] cfg lapic_enable=", (u64)g_cfg.lapic_enable);
        log_dec("[apic] cfg ioapic_init=", (u64)g_cfg.ioapic_init);
        log_dec("[apic] cfg apic_route=", (u64)g_cfg.apic_route);
        log_dec("[apic] cfg apic_route_legacy=", (u64)g_cfg.apic_route_legacy);
        log_dec("[apic] cfg apic_diag=", (u64)g_cfg.apic_diag);
        return;
    }

    g_log->warn("[apic] FUCK module (fuck:config) not found; defaults in effect");
}

static const acpi_sdt_header *find_table_from_rsdt(const acpi_sdt_header *rsdt, const char sig[4], u32 entry_size) {
    if (!rsdt || rsdt->length < sizeof(acpi_sdt_header)) return NULL;
    if (checksum8((const u8 *)rsdt, rsdt->length) != 0) return NULL;

    u32 count = (rsdt->length - sizeof(acpi_sdt_header)) / entry_size;
    const u8 *entries = (const u8 *)rsdt + sizeof(acpi_sdt_header);
    for (u32 i = 0; i < count; i++) {
        u64 addr;
        if (entry_size == 4) {
            addr = (u64)((const u32 *)entries)[i];
        } else {
            addr = ((const u64 *)entries)[i];
        }
        if (!addr) continue;

        const acpi_sdt_header *hdr = (const acpi_sdt_header *)(uintptr_t)addr;
        if (hdr->length >= sizeof(acpi_sdt_header) && memeq(hdr->signature, sig, 4)) {
            if (checksum8((const u8 *)hdr, hdr->length) == 0) return hdr;
        }
    }
    return NULL;
}

static const acpi_madt *find_madt(const void *rsdp_addr) {
    if (!rsdp_addr) return NULL;

    const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)rsdp_addr;
    if (!memeq(rsdp->signature, "RSD PTR ", 8)) return NULL;
    if (checksum8((const u8 *)rsdp, 20) != 0) return NULL;

    if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_v2) && rsdp->xsdt_addr) {
        if (checksum8((const u8 *)rsdp, rsdp->length) == 0) {
            const acpi_sdt_header *xsdt = (const acpi_sdt_header *)(uintptr_t)rsdp->xsdt_addr;
            const acpi_sdt_header *madt = find_table_from_rsdt(xsdt, "APIC", 8);
            if (madt) return (const acpi_madt *)madt;
        }
    }

    if (rsdp->rsdt_addr) {
        const acpi_sdt_header *rsdt = (const acpi_sdt_header *)(uintptr_t)(u64)rsdp->rsdt_addr;
        const acpi_sdt_header *madt = find_table_from_rsdt(rsdt, "APIC", 4);
        if (madt) return (const acpi_madt *)madt;
    }

    return NULL;
}

/* 查找 HPET 表 (apic_diag 用; 与 find_madt 同构, 仅签名不同) */
static const acpi_hpet *find_hpet(const void *rsdp_addr) {
    if (!rsdp_addr) return NULL;
    const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)rsdp_addr;
    if (!memeq(rsdp->signature, "RSD PTR ", 8)) return NULL;
    if (checksum8((const u8 *)rsdp, 20) != 0) return NULL;
    if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_v2) && rsdp->xsdt_addr) {
        if (checksum8((const u8 *)rsdp, rsdp->length) == 0) {
            const acpi_sdt_header *xsdt = (const acpi_sdt_header *)(uintptr_t)rsdp->xsdt_addr;
            const acpi_sdt_header *hpet = find_table_from_rsdt(xsdt, "HPET", 8);
            if (hpet) return (const acpi_hpet *)hpet;
        }
    }
    if (rsdp->rsdt_addr) {
        const acpi_sdt_header *rsdt = (const acpi_sdt_header *)(uintptr_t)(u64)rsdp->rsdt_addr;
        const acpi_sdt_header *hpet = find_table_from_rsdt(rsdt, "HPET", 4);
        if (hpet) return (const acpi_hpet *)hpet;
    }
    return NULL;
}

static void parse_madt_entries(const acpi_madt *madt, struct madt_info *info) {
    const u8 *base = (const u8 *)madt;
    u32 off = (u32)sizeof(acpi_madt);

    while (off + 2 <= madt->hdr.length) {
        const u8 *entry = base + off;
        u8 type = entry[0];
        u8 len = entry[1];
        if (len < 2 || off + len > madt->hdr.length) {
            g_log->warn("[apic] malformed MADT entry");
            return;
        }

        if (type == 0 && len >= 8) {
            /* LAPIC entry */
            u8 apic_id = entry[3];
            u32 flags = *(const u32 *)(const void *)(entry + 4);
            log_hex("[apic] CPU LAPIC id=", apic_id);
            if (flags & 1u) {
                info->lapic_count++;
                if (info->lapic_count <= MAX_LAPIC) {
                    info->lapics[info->lapic_count - 1].apic_id = apic_id;
                    info->lapics[info->lapic_count - 1].flags = flags;
                }
            }
        } else if (type == 1 && len >= 12) {
            /* IOAPIC entry */
            u8 ioapic_id = entry[2];
            u32 addr = *(const u32 *)(const void *)(entry + 4);
            u32 gsi = *(const u32 *)(const void *)(entry + 8);
            log_hex("[apic] IOAPIC id=", ioapic_id);
            log_hex("[apic] IOAPIC addr=", addr);
            log_hex("[apic] IOAPIC gsi_base=", gsi);
            if (info->ioapic_count < MAX_IOAPIC) {
                struct ioapic_info *io = &info->ioapics[info->ioapic_count];
                io->id = ioapic_id;
                io->gsi_base = gsi;
                io->mmio_phys = (u64)addr;
                io->mmio = (volatile u32 *)(uintptr_t)(u64)addr; /* HHDM 直映射, 后续映射修正 */
                io->gsi_count = 0;
                info->ioapic_count++;
            }
        } else if (type == 2 && len >= 10) {
            /* ISO (Interrupt Source Override) entry */
            u8 source = entry[3];
            u32 gsi = *(const u32 *)(const void *)(entry + 4);
            u16 flags = *(const u16 *)(const void *)(entry + 8);
            info->iso_count++;
            log_hex("[apic] ISO source=", source);
            log_hex("[apic] ISO gsi=", gsi);
            log_hex("[apic] ISO flags=", flags);
            if (info->iso_count <= MAX_ISO) {
                struct iso_info *iso = &info->isos[info->iso_count - 1];
                iso->bus_source = source;
                iso->gsi = gsi;
                iso->flags = flags;
            }
        } else if (type == 5 && len >= 12) {
            u64 override = *(const u64 *)(const void *)(entry + 4);
            info->lapic_phys = override;
            log_hex("[apic] LAPIC override=", override);
        }

        off += len;
    }
}

/* ---- IOAPIC MMIO 访问 ---- *
 * IOAPIC 寄存器通过 MMIO 访问, 地址空间为 IOREGSEL(0x00) + IOWIN(0x10).
 * 真机 IOAPIC 地址通常在 0xFEC00000, HHDM 通常覆盖此低 4G 地址。
 * 写入 IOREGSEL 选择寄存器号, 读/写 IOWIN 访问数据。
 */

#define IOAPIC_IOREGSEL 0x00
#define IOAPIC_IOWIN    0x10

static u32 ioapic_read(volatile u32 *base, u8 reg) {
    base[IOAPIC_IOREGSEL / 4] = (u32)reg;
    apic_mb();  /* 寄存器选择必须先于数据读取 */
    return base[IOAPIC_IOWIN / 4];
}

static void ioapic_write(volatile u32 *base, u8 reg, u32 val) {
    base[IOAPIC_IOREGSEL / 4] = (u32)reg;
    apic_mb();  /* 寄存器选择必须先于数据写入 */
    base[IOAPIC_IOWIN / 4] = val;
}

/* IOAPIC 重定向表项 (64-bit, 分两次 32-bit 读写, 82093AA 规范布局)
 *   bits 0-7:   interrupt vector (0x20-0xFF)
 *   bits 8-10:  delivery mode: 0=fixed, 1=lowest, 2=SMI, 4=NMI, 5=INIT, 7=ExtINT
 *   bit  11:    destination mode: 0=physical, 1=logical
 *   bit  12:    delivery status (RO)
 *   bit  13:    polarity: 0=active high, 1=active low
 *   bit  14:    remote IRR (RO, level-triggered only)
 *   bit  15:    trigger mode: 0=edge, 1=level
 *   bit  16:    mask: 0=enabled, 1=masked
 *   bits 17-55: reserved
 *   bits 56-63: destination APIC ID (physical mode) or MDA (logical mode)
 * 注: 历史版本曾将 polarity/trigger/mask 错置于 bit 11/13/14,
 *     导致 mask 写入只读 remote IRR 位而从未生效 —— 勿回退。
 */
static u64 ioapic_read_redir(volatile u32 *base, u8 entry) {
    u32 lo = ioapic_read(base, (u8)(0x10 + entry * 2));
    u32 hi = ioapic_read(base, (u8)(0x10 + entry * 2 + 1));
    return ((u64)hi << 32) | (u64)lo;
}

static void ioapic_write_redir(volatile u32 *base, u8 entry, u64 val) {
    ioapic_write(base, (u8)(0x10 + entry * 2), (u32)(val & 0xffffffffu));
    ioapic_write(base, (u8)(0x10 + entry * 2 + 1), (u32)(val >> 32));
}

/* ---- B7: GSI 路由辅助 ---- */

/* 合成重定向表项: vector + ISO 极性/触发 + 物理目的 APIC ID + mask 位 */
static u64 redir_compose(u32 vector, u16 iso_flags, u32 dest_apic_id, int masked) {
    u64 entry = (u64)(vector & 0xFFu);
    /* delivery mode = fixed (0), destination mode = physical (0) */

    /* 极性: bit 13. ISA 默认 active-high (0).
     * ISO flags bit 0-1: 0=conforms, 1=active-high, 2=reserved, 3=active-low */
    if ((iso_flags & 0x03u) == 0x03u) entry |= (1ULL << 13);

    /* 触发: bit 15. ISA 默认 edge (0).
     * ISO flags bit 2-3: 0=conforms, 1=edge, 2=reserved, 3=level */
    if ((iso_flags & 0x0Cu) == 0x0Cu) entry |= (1ULL << 15);

    entry |= ((u64)dest_apic_id << 56);
    if (masked) entry |= (1ULL << 16);
    return entry;
}

/* 找覆盖指定 GSI 的 IOAPIC (多 IOAPIC 按 GSI 范围划分) */
static struct ioapic_info *ioapic_for_gsi(struct madt_info *info, u32 gsi) {
    for (u32 i = 0; i < info->ioapic_count; i++) {
        struct ioapic_info *io = &info->ioapics[i];
        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->gsi_count) return io;
    }
    return NULL;
}

/* ISA IRQ → GSI 解析: 优先 MADT ISO override, 缺省恒等 (QEMU ISA 惯例) */
static u32 isa_irq_to_gsi(u8 isa_irq, u16 *flags_out,
                          const struct iso_info *isos, u32 iso_count) {
    for (u32 i = 0; i < iso_count; i++) {
        if (isos[i].bus_source == isa_irq) {
            *flags_out = isos[i].flags;
            return isos[i].gsi;
        }
    }
    *flags_out = 0;
    return (u32)isa_irq;
}

/* 编程单个 GSI 的重定向项 (unmask=1 放开投递, 0 保持屏蔽) */
static int ioapic_program_gsi(struct madt_info *info, u32 gsi,
                              u32 vector, u16 iso_flags, int unmask) {
    struct ioapic_info *io = ioapic_for_gsi(info, gsi);
    if (!io || !io->mmio) return -1;
    u8 pin = (u8)(gsi - io->gsi_base);
    ioapic_write_redir(io->mmio, pin,
                       redir_compose(vector, iso_flags, g_lapic_id, !unmask));
    return 0;
}

/* 全 IOAPIC 全条目屏蔽 (降级路径) — 保留 vector 信息, 仅置 mask 位 */
static void ioapic_mask_all(struct madt_info *info) {
    for (u32 i = 0; i < info->ioapic_count; i++) {
        struct ioapic_info *io = &info->ioapics[i];
        if (!io->mmio) continue;
        for (u32 pin = 0; pin < io->gsi_count && pin < 64; pin++) {
            u64 entry = ioapic_read_redir(io->mmio, (u8)pin);
            entry |= (1ULL << 16);
            ioapic_write_redir(io->mmio, (u8)pin, entry);
        }
    }
}

/* 重定向表回读 dump (验证证据) */
static void ioapic_dump_table(struct madt_info *info) {
    for (u32 i = 0; i < info->ioapic_count; i++) {
        struct ioapic_info *io = &info->ioapics[i];
        if (!io->mmio) continue;
        g_log->info("[apic] IOREDTBL readback:");
        for (u32 pin = 0; pin < io->gsi_count && pin < 24; pin++) {
            u64 entry = ioapic_read_redir(io->mmio, (u8)pin);
            u32 vec = (u32)(entry & 0xFFu);
            u32 masked = (u32)((entry >> 16) & 1u);
            log_dec("[apic]   pin=", pin);
            log_hex("[apic]     vec=", vec);
            log_dec("[apic]     mask=", masked);
        }
    }
}

/* ---- IOAPIC 初始化 ---- *
 * 逐条目编程重定向表: ISA IRQ→vector 映射 + mask all。
 * 真机关键: 处理 MADT ISO (IRQ0→GSI2 是最常见的 override)。
 * PIC 兼容模式: vector = 0x20 + IRQ (与 PIC remap 布局一致)。
 *
 * **QEMU 无法测试但真机必需的代码路径**:
 *   1. ISO 处理: 真机几乎总有 IRQ0→GSI2 override, QEMU 默认有 1 条
 *   2. 极性标志: 真机 ISA IRQ0 (timer) 必须设 active-high,
 *      但 ISO 可能指定 active-low (PCIe SCI 等)
 *   3. 多 IOAPIC: 真机可能有 2+ IOAPIC, 每个 GSI 范围不同
 */

static void ioapic_init_one(struct ioapic_info *io, u64 hhdm_offset,
                             const struct iso_info *isos, u32 iso_count,
                             void *(*mm_map_mmio_fn)(u64, u64)) {
    /* 映射 IOAPIC MMIO — 真机地址通常在 0xFECxxxxx, HHDM 通常覆盖,
     * 但使用 mm_map_mmio 更安全 (PCD|PWT 属性) */
    if (mm_map_mmio_fn) {
        void *mapped = mm_map_mmio_fn(io->mmio_phys, 0x1000);
        if (mapped) {
            io->mmio = (volatile u32 *)mapped;
            g_log->info("[apic] IOAPIC mapped via mm_map_mmio");
        } else {
            g_log->warn("[apic] mm_map_mmio failed for IOAPIC; trying HHDM");
            io->mmio = (volatile u32 *)(uintptr_t)(hhdm_offset + io->mmio_phys);
        }
    } else {
        io->mmio = (volatile u32 *)(uintptr_t)(hhdm_offset + io->mmio_phys);
    }

    /* 读取 IOAPIC 版本寄存器获取最大重定向条目数 */
    u32 ver = ioapic_read(io->mmio, 0x01);
    u32 max_redir = ((ver >> 16) & 0xff) + 1;
    io->gsi_count = max_redir;
    log_hex("[apic] IOAPIC VER=", ver);
    log_hex("[apic] IOAPIC max_redir=", max_redir);

    /* 读取 IOAPIC ID 验证 */
    u32 id_reg = ioapic_read(io->mmio, 0x00);
    log_hex("[apic] IOAPIC ID reg=", id_reg);

    /* 构建默认 ISA IRQ→GSI→vector 映射表 (identity map + ISO override)
     * 默认: GSI = ISA_IRQ (0-15), vector = 0x20 + ISA_IRQ
     * ISO  override: 修改特定 IRQ 的 GSI 和极性/触发模式 */
    u32 gsi_to_vector[256];   /* GSI → vector 映射 */
    u32 gsi_flags[256];       /* GSI → polar/trigger flags from ISO */
    for (u32 i = 0; i < 256; i++) {
        gsi_to_vector[i] = 0;
        gsi_flags[i] = 0;
    }

    /* 先建立 identity 映射: GSI 0-15 → vector 0x20-0x2F */
    for (u32 gsi = 0; gsi < 16; gsi++) {
        gsi_to_vector[gsi] = 0x20 + gsi;
    }

    /* 应用 ISO override */
    for (u32 i = 0; i < iso_count; i++) {
        const struct iso_info *iso = &isos[i];
        u8 src = iso->bus_source;
        u32 gsi = iso->gsi;
        if (gsi < 256) {
            gsi_to_vector[gsi] = 0x20 + src;  /* vector 仍基于 ISA IRQ 编号 */
            gsi_flags[gsi] = iso->flags;
            g_log->info("[apic] ISO: ISA IRQ -> GSI override");
            log_hex("[apic]   ISA IRQ=", src);
            log_hex("[apic]   GSI=", gsi);
            log_hex("[apic]   vector=", 0x20 + src);
        }
    }

    /* 编程重定向表: 全部 mask, 编入正确的 vector/极性/触发模式/目标 APIC */
    for (u32 pin = 0; pin < max_redir && pin < 256; pin++) {
        u32 gsi = io->gsi_base + pin;
        u64 entry = 0;

        if (gsi < 256 && gsi_to_vector[gsi] != 0) {
            u32 vec = gsi_to_vector[gsi];
            u16 flags = gsi_flags[gsi];

            /* vector + ISO 极性/触发 + 真实 BSP LAPIC ID, 保持 mask */
            entry = redir_compose(vec, flags, g_lapic_id, 1);
        } else {
            entry = (1ULL << 16);  /* 无映射条目: 纯 mask (bit16, vector=0 无害) */
        }

        ioapic_write_redir(io->mmio, (u8)pin, entry);
    }

    g_log->info("[apic] IOAPIC redirect table programmed (all masked)");
}

/* ---- LAPIC 使能 ---- *
 * 真机必须:
 *   1. 写 IA32_APIC_BASE MSR 启用 bit 11 (global enable)
 *   2. 写 LAPIC SVR 寄存器设置 Spurious Interrupt Vector (建议 0xFF)
 *   3. 设置 SVR bit 8 (APIC software enable)
 *
 * **QEMU 无法测试但真机必需**: LAPIC SVR 设置 — QEMU 默认已启用 LAPIC,
 * 但实机 BIOS 可能留 LAPIC 为 software-disable 状态。
 */

static void lapic_enable(void) {
    volatile u32 *lapic = g_lapic;

    /* 确保 IA32_APIC_BASE 的 global enable 和 BSP 位已设置 */
    u64 apic_base = rdmsr(0x1B);
    apic_base |= (1ULL << 11);  /* global enable */
    /* 保持 BSP 位不变 (bit 8) */
    wrmsr(0x1B, apic_base);
    g_log->info("[apic] LAPIC global enable set in MSR");

    /* 读当前 SVR */
    u32 svr = lapic[0xF0 / 4];
    log_hex("[apic] LAPIC SVR before=", svr);

    /* 设置 Spurious Interrupt Vector = 0xFF + APIC software enable (bit 8) */
    svr = 0xFF | (1u << 8);
    lapic[0xF0 / 4] = svr;
    apic_mb();

    /* 回读验证 */
    svr = lapic[0xF0 / 4];
    log_hex("[apic] LAPIC SVR after=", svr);
    if (svr & (1u << 8)) {
        g_log->info("[apic] LAPIC software enabled");
    } else {
        g_log->warn("[apic] LAPIC SVR bit8 not set — hardware may not support");
    }

    /* 读 LAPIC ID 和 version 做诊断 dump */
    log_hex("[apic] LAPIC ID=", lapic[0x20 / 4]);
    log_hex("[apic] LAPIC LDR=", lapic[0xD0 / 4]);
    log_hex("[apic] LAPIC version=", lapic[0x30 / 4]);
}

/* LAPIC EOI — 写 0 到 EOI 寄存器 (offset 0xB0)。
 * B7 阶段2: 本函数作为 EOI 钩子经 kernel_api.register_apic_eoi 注册给 UTSM,
 * idt_handler 在两个 IRQ 分支的分发末尾调用 (钩子签名 void(*)(void) 匹配)。
 * 调用点: IRQ 上下文, 单核 BSP interrupt gate (IF=0), 无嵌套, 无需锁。 */
static __inline__ void lapic_eoi(void) {
    if (g_lapic) {
        g_lapic[0xB0 / 4] = 0;
        apic_mb();
    }
}

/* ---- B7 diag: HPET 绝对时基 (tick 2x 定论实验) ---- *
 * 选型理由: TSC 校准 (PIT mode0 回绕法) 与被测对象 (PIT mode3 投递频率)
 * 共享 PIT 硬件, 循环论证无法定论 2x 根因。HPET 主计数器是独立时基
 * (QEMU q35: 100MHz, period=10000fs), 32 位回绕周期 ~42.9s, 对 ms 级
 * 窗口充裕。只设 GEN_CONF bit0 (overall enable); 绝不置 bit1
 * (legacy replacement route 会抢 PIT IRQ0/RTC IRQ8 路由, 破坏被测链路)。
 */

static volatile u32 *g_hpet;          /* HPET MMIO 基址 (HHDM), 0=不可用 */
static u32 g_hpet_period_fs;          /* 主计数器周期 (飞秒), GCAP_ID[63:32] */

#define HPET_GCAP_ID   0x00
#define HPET_GEN_CONF  0x10
#define HPET_MAIN_CNT  0xF0

static u64 hpet_read64(u32 off) {
    volatile u32 *p = (volatile u32 *)((volatile u8 *)g_hpet + off);
    u32 lo = p[0];
    u32 hi = p[1];
    return ((u64)hi << 32) | lo;
}

static void hpet_write64(u32 off, u64 v) {
    volatile u32 *p = (volatile u32 *)((volatile u8 *)g_hpet + off);
    p[0] = (u32)(v & 0xffffffffu);
    p[1] = (u32)(v >> 32);
}

static __inline__ u32 hpet_main32(void) {
    return *(volatile u32 *)((volatile u8 *)g_hpet + HPET_MAIN_CNT);
}

/* 初始化 HPET 时基; 成功返回 0 且 g_hpet/g_hpet_period_fs 有效 */
static int hpet_diag_init(const struct dkm_kernel_api *api) {
    const acpi_hpet *hpet = find_hpet(api->rsdp_address);
    if (!hpet) {
        g_log->warn("[apic] diag: HPET table not found");
        return -1;
    }
    if (hpet->addr_space_id != 0 || !hpet->base_addr) {
        g_log->warn("[apic] diag: HPET not memory-mapped");
        return -1;
    }
    g_hpet = (volatile u32 *)(uintptr_t)(api->hhdm_offset + hpet->base_addr);
    u64 gcap = hpet_read64(HPET_GCAP_ID);
    g_hpet_period_fs = (u32)(gcap >> 32);
    if (!g_hpet_period_fs || g_hpet_period_fs > 100000000u) {
        g_log->warn("[apic] diag: HPET period implausible");
        g_hpet = 0;
        return -1;
    }
    log_hex("[apic] diag: HPET base=", hpet->base_addr);
    log_dec("[apic] diag: HPET period(fs)=", g_hpet_period_fs);

    /* overall enable (bit0); legacy replacement route (bit1) 保持 0 */
    u64 conf = hpet_read64(HPET_GEN_CONF);
    if (!(conf & 1u)) {
        hpet_write64(HPET_GEN_CONF, conf | 1u);
        apic_mb();
    }
    g_log->info("[apic] diag: HPET main counter enabled");
    return 0;
}

/* HPET 主计数器差值 → 微秒 (全程 u64 整数, -msoft-float 环境无浮点) */
static u64 hpet_delta_us(u32 h0, u32 h1) {
    /* 无符号回绕安全: u32 减法自动回绕 (42.9s 周期, 窗口 << 此值) */
    u64 ticks = (u64)(u32)(h1 - h0);
    /* ticks * period_fs / 1e9 = us; period_fs 典型 10000, 500ms 窗口
     * ticks≈5e7, 乘积 5e11 << 2^64, 溢出安全 */
    return (ticks * (u64)g_hpet_period_fs) / 1000000000ull;
}

/* ---- B7 diag: PS/2 鼠标流 (IRQ12 持续投递验证) ---- *
 * UTSM 阶段 IRQ12 无属主 (mouseInit 是 DSK 阶段模块), diag handler 独占;
 * DSK mouseInit 之后经 irq_register(12) 覆盖本 handler 并完整复位鼠标,
 * 无状态冲突。所有单步带超时, 失败只记日志不阻塞。 */

static volatile u32 g_mouse_pkt_count;

static int ps2_wait_ibf(void) {
    u32 t = 100000;
    while (t--) { if (!(inb(0x64) & 0x02)) return 0; }
    return -1;
}

static int ps2_wait_obf(void) {
    u32 t = 100000;
    while (t--) { if (inb(0x64) & 0x01) return 0; }
    return -1;
}

static void ps2_drain(void) {
    while (inb(0x64) & 0x01) { (void)inb(0x60); }
}

/* 经 0xD4 通道写鼠标命令并等 ACK(0xFA); 失败返回 -1 */
static int ps2_aux_cmd(u8 cmd) {
    if (ps2_wait_ibf() != 0) return -1;
    outb(0x64, 0xD4);
    if (ps2_wait_ibf() != 0) return -1;
    outb(0x60, cmd);
    if (ps2_wait_obf() != 0) return -1;
    return inb(0x60) == 0xFA ? 0 : -1;
}

/* IRQ12 诊断 handler: 只读数据计数, EOI 交给 idt_handler 分发路径
 * (PIC 模式: 8259 EOI; route 模式: 钩子 LAPIC EOI + 无害 8259 EOI)。
 * 返回 0 — 不自负 EOI, 与 ps2kbd/e1000 handler 同一约定。 */
static int apic_mouse_diag_handler(u8 vector) {
    (void)vector;
    u8 st = inb(0x64);
    if ((st & 0x01) && (st & 0x20)) {   /* OBF 且 AUX 位置位 = 鼠标数据 */
        u8 data = inb(0x60);            /* 必须读, 否则 OBF 保持阻塞后续包 */
        g_mouse_pkt_count++;
        if (g_mouse_pkt_count <= 6) {
            g_log->info("[apic] diag: mouse packet byte");
            log_hex("[apic] diag:   data=", data);
        }
    }
    return 0;
}

/* IRQ1 诊断 handler (键盘流计数): ps2kbd demo 窗口只有 ~10ms 数量级,
 * 自动化无法抓住; diag 在 stage0 自建 3s 观察窗验证 IRQ1 持续投递。
 * stage3 ps2kbd 注册时覆盖本 handler, 无冲突。 */
static volatile u32 g_kbd_pkt_count;

static int apic_kbd_diag_handler(u8 vector) {
    (void)vector;
    u8 st = inb(0x64);
    if ((st & 0x01) && !(st & 0x20)) {  /* OBF 且 AUX 位清零 = 键盘数据 */
        u8 data = inb(0x60);            /* 读走扫描码 (窗内按键, 无消费者) */
        g_kbd_pkt_count++;
        if (g_kbd_pkt_count <= 6) {
            g_log->info("[apic] diag: kbd scancode byte");
            log_hex("[apic] diag:   sc=", data);
        }
    }
    return 0;
}

/* AUX 端口初始化: 启用 AUX 时钟 + 采样率 100 + 数据报告, 最后才开 AUX IRQ。
 * 顺序要点: AUX IRQ (cfg bit1) 必须在全部命令/ACK 交互完成后才使能 —
 * 否则 ACK 字节 (0xFA) 到达即触发 IRQ12 被 diag handler 读走, 轮询侧
 * 永远等不到 ACK (实测 F3 nack 即此竞态: handler 日志 data=0xfa)。
 * 单步失败只记日志继续 (与 mouseInit 同一安全准则)。 */
static void ps2_aux_enable_stream(void) {
    ps2_drain();

    if (ps2_wait_ibf() != 0) { g_log->warn("[apic] diag: PS2 ibf timeout"); return; }
    outb(0x64, 0x20);                   /* 读配置字节 */
    if (ps2_wait_obf() != 0) { g_log->warn("[apic] diag: PS2 cfg read timeout"); return; }
    u8 cfg = inb(0x60);
    cfg &= (u8)~0x02;                   /* bit1=0: AUX IRQ 先关闭 (轮询期) */
    cfg &= (u8)~0x20;                   /* bit5=0: AUX clock enable */
    cfg |= 0x01;                        /* bit0=1: KBD IRQ 保持使能 (键盘流用) */

    if (ps2_wait_ibf() != 0) return;
    outb(0x64, 0x60);                   /* 写配置字节 */
    if (ps2_wait_ibf() != 0) return;
    outb(0x60, cfg);
    log_hex("[apic] diag: PS2 cfg byte=", cfg);

    if (ps2_wait_ibf() != 0) return;
    outb(0x64, 0xA8);                   /* 启用 AUX 端口 */

    if (ps2_aux_cmd(0xF3) != 0) { g_log->warn("[apic] diag: mouse F3 nack"); }
    else if (ps2_aux_cmd(100) != 0) { g_log->warn("[apic] diag: mouse rate nack"); }
    if (ps2_aux_cmd(0xF4) != 0) { g_log->warn("[apic] diag: mouse F4 nack"); return; }
    g_log->info("[apic] diag: PS/2 mouse data reporting enabled");

    ps2_drain();                        /* 清 F4 ACK 后可能挂起的首个流字节 */

    /* 全部交互完成, 现在才开 AUX IRQ — 之后鼠标字节全走 IRQ12 handler */
    if (ps2_wait_ibf() != 0) return;
    outb(0x64, 0x20);
    if (ps2_wait_obf() != 0) return;
    cfg = inb(0x60);
    cfg |= 0x02;                        /* bit1=1: AUX IRQ enable */
    if (ps2_wait_ibf() != 0) return;
    outb(0x64, 0x60);
    if (ps2_wait_ibf() != 0) return;
    outb(0x60, cfg);
    log_hex("[apic] diag: PS2 cfg byte(final)=", cfg);
}

/* ---- B7 阶段2: PIT tick 路由验证 ---- *
 * vector 0x20 落在 idt_handler 的 PIC 分支 (vector 32-47) — 与键盘 0x21/
 * 鼠标 0x2C/网卡 0x2B 同一代码路径。分发顺序: handler(本函数) → 钩子
 * LAPIC EOI → 8259 EOI (PIC 全屏蔽下为无害 no-op)。
 * 本 handler 返回 0 不自负 EOI: 由 UTSM 钩子写 LAPIC EOI 寄存器清除
 * ISR 位; 重复投递 (计数>=2) 即证明"legacy 向量分支 + 钩子 EOI"可持续,
 * 这正是键盘/鼠标/网卡将要走的路径 (阶段1 的 0xE0+自EOI 无法证明此路径)。
 * handler 是普通 C 函数 (SysV ABI), isr stub 已完成现场保存/iret。
 */
static int apic_tick_handler(u8 vector) {
    (void)vector;
    g_tick_count++;
    if (g_tick_count <= 8) {
        g_log->info("[apic] ioapic tick (PIT GSI -> LAPIC -> IDT vec 0x20, hook EOI)");
    }
    return 0;
}

static u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static u64 g_tsc_per_ms;

/* PIT ch0 mode 0 单发 (~10ms, 11932 counts) 校准 TSC。
 * 必须在把 PIT 改为 mode 3 周期模式之前调用。 */
static void tsc_calibrate(void) {
    outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    outb(0x40, 0x9C);       /* 11932 low = ~10ms */
    outb(0x40, 0x2E);       /* 11932 high */
    u64 tsc_start = rdtsc();
    u16 prev = 0;
    u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;  /* 回绕 = 10ms 到 */
        prev = cur;
        loops++;
    }
    u64 tsc_end = rdtsc();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static void tsc_delay_ms(u32 ms) {
    if (!g_tsc_per_ms) {
        for (volatile u32 i = 0; i < 100000 * ms; i++) __asm__ volatile("pause");
        return;
    }
    u64 target = g_tsc_per_ms * ms;
    u64 start = rdtsc();
    while (rdtsc() - start < target) __asm__ volatile("pause");
}

/* ---- B7 阶段2: 路由切换主流程 ---- *
 * 成功: PIC IMR 全 1, IOAPIC 拥有路由, EOI 钩子保持注册, tick 验证后
 *       tick 线重新 mask (保留路由表内容), legacy 线按 apic_route_legacy。
 * 失败: IOAPIC 全 mask + 恢复原 PIC IMR + 注销 tick handler + 注销 EOI
 *       钩子, 与切换前状态完全一致 (现状不回归)。
 */
static void apic_route_switch(const struct dkm_kernel_api *api,
                              struct madt_info *info) {
    g_log->info("[apic] apic_route=1: attempting IOAPIC routing takeover");

    if (!g_lapic) {
        g_log->warn("[apic] route abort: LAPIC MMIO unavailable; PIC retained");
        return;
    }
    if (info->ioapic_count == 0) {
        g_log->warn("[apic] route abort: no IOAPIC; PIC retained");
        return;
    }
    if (!api->irq_register) {
        g_log->warn("[apic] route abort: irq_register missing; PIC retained");
        return;
    }

    /* 0) B7 阶段2 前提: EOI 钩子接口必须存在 (旧内核无此字段 → 判空中止)。
     *    无钩子时 legacy 向量 (0x20-0x2F) 在 IOAPIC 路由下只能 one-shot,
     *    路由接管不成立 — 宁可保留 PIC 也不进入半残状态。 */
    if (!api->register_apic_eoi) {
        g_log->warn("[apic] route abort: register_apic_eoi missing (old kernel); PIC retained");
        return;
    }
    api->register_apic_eoi(lapic_eoi);
    g_log->info("[apic] LAPIC EOI hook registered to UTSM");

    /* 1) 保存 PIC IMR (降级恢复用) */
    u8 imr1 = inb(0x21);
    u8 imr2 = inb(0xA1);
    log_hex("[apic] PIC IMR saved master=", imr1);
    log_hex("[apic] PIC IMR saved slave=", imr2);

    /* 2) TSC 校准 (PIT 尚在 firmware/driver 遗留状态, 先校准再改模式)。
     *    diag: HPET 绝对时基对拍本窗口 — 校准窗口标称 10ms, HPET 实测
     *    若 ~20000us 则 PIT mode0 实际跑了 20ms (TSC 测半), ~10000us
     *    则校准正确, tick 2x 根因另在他处。 */
    int hpet_ok = 0;
    if (g_cfg.apic_diag) {
        hpet_ok = (hpet_diag_init(api) == 0);
    }
    u32 hc0 = 0;
    if (hpet_ok) hc0 = hpet_main32();
    tsc_calibrate();
    if (hpet_ok) {
        u32 hc1 = hpet_main32();
        u64 calib_us = hpet_delta_us(hc0, hc1);
        log_dec("[apic] diag: calib window(us)=", calib_us);
        g_log->info("[apic] diag: (nominal 10000us; ~20000 => TSC half-measured)");
    }
    log_hex("[apic] tsc_per_ms=", g_tsc_per_ms);

    /* 3) 注册 tick handler @ vector 0x20 — PIC 分支 (legacy 向量路径),
     *    handler 返回 0, LAPIC EOI 由 UTSM 钩子完成 */
    if (api->irq_register(0x20, (void *)(uintptr_t)apic_tick_handler) != 0) {
        g_log->warn("[apic] route abort: irq_register(0x20) failed");
        api->register_apic_eoi(NULL);
        return;
    }
    g_log->info("[apic] tick handler registered at vector 0x20 (PIC branch, hook EOI)");

    /* 4) PIT tick GSI 解析: MADT ISO source 0 (QEMU q35: GSI2), 缺省恒等 */
    u16 tick_flags = 0;
    u32 tick_gsi = isa_irq_to_gsi(0, &tick_flags, info->isos, info->iso_count);
    log_hex("[apic] tick GSI=", tick_gsi);
    log_hex("[apic] tick GSI flags=", tick_flags);

    /* 5) 编程 tick 重定向项 (vector 0x20, 放开) */
    if (ioapic_program_gsi(info, tick_gsi, 0x20, tick_flags, 1) != 0) {
        g_log->warn("[apic] route abort: tick GSI not covered by any IOAPIC");
        api->irq_register(0x20, (void *)0);
        api->register_apic_eoi(NULL);
        return;
    }
    g_log->info("[apic] tick redirection unmasked (vec 0x20)");

    /* 6) legacy 线 (apic_route_legacy): EOI 钩子已就位, 持续投递成立。
     *    ISA 线 (IRQ1/12) 按 ISO, 缺省 edge/active-high;
     *    PCI 线 (IRQ11 e1000) 无 ISO 覆盖时强制 level/active-low
     *    (PCI INTx 规范; flags=0xF = pol bit0-1=3 active-low + trig bit2-3=3
     *    level)。e1000 handler 读 ICR 清源 → 电平 deassert → 钩子 EOI。 */
    if (g_cfg.apic_route_legacy) {
        u16 f1 = 0, f12 = 0, f11 = 0;
        u32 gsi1 = isa_irq_to_gsi(1, &f1, info->isos, info->iso_count);
        u32 gsi12 = isa_irq_to_gsi(12, &f12, info->isos, info->iso_count);
        u32 gsi11 = isa_irq_to_gsi(11, &f11, info->isos, info->iso_count);
        if (!f11) f11 = 0x0F;
        ioapic_program_gsi(info, gsi1, 0x21, f1, 1);    /* 键盘 */
        ioapic_program_gsi(info, gsi12, 0x2C, f12, 1);  /* 鼠标 */
        ioapic_program_gsi(info, gsi11, 0x2B, f11, 1);  /* e1000 (QEMU 默认 IRQ11) */
        g_log->info("[apic] legacy IRQ1/12/11 unmasked (sustained delivery via EOI hook)");
        log_hex("[apic]   kbd gsi=", gsi1);
        log_hex("[apic]   mouse gsi=", gsi12);
        log_hex("[apic]   e1000 gsi=", gsi11);
        log_hex("[apic]   e1000 flags=", f11);
    }

    /* 7) LAPIC TPR=0 全放行 */
    g_lapic[0x80 / 4] = 0;
    apic_mb();
    log_hex("[apic] LAPIC TPR=", g_lapic[0x80 / 4]);

    /* 8) PIC 全屏蔽 (先 slave 后 master, 避免 cascade 窗口; QEMU 无功能 IMCR) */
    outb(0xA1, 0xFF);
    io_wait();
    outb(0x21, 0xFF);
    io_wait();
    g_log->info("[apic] PIC fully masked; IOAPIC owns IRQ routing");

    /* 9) PIT ch0 mode 3 周期 ~100Hz (divisor 11932), 产生真实 tick 源 */
    outb(0x43, 0x36);
    outb(0x40, 0x9C);
    outb(0x40, 0x2E);
    g_log->info("[apic] PIT ch0 mode3 100Hz started");

    /* 10) 500ms 验证窗口 (+ diag: HPET 实测窗口与 tick 频率定论) */
    u32 hv0 = 0;
    if (hpet_ok) hv0 = hpet_main32();
    u32 before = g_tick_count;
    tsc_delay_ms(500);
    u32 ticks = g_tick_count - before;
    if (hpet_ok) {
        u32 hv1 = hpet_main32();
        u64 win_us = hpet_delta_us(hv0, hv1);
        log_dec("[apic] diag: verify window(us)=", win_us);
        if (win_us) {
            u64 hz_x10 = (u64)ticks * 10000000ull / win_us;
            log_dec("[apic] diag: tick rate(Hz x10)=", hz_x10);
        }
        /* tick 2x 定论分支:
         *   win≈500ms 且 ticks≈100 → PIT mode3 在 IOAPIC edge 下双触发 (200Hz 真实)
         *   win≈1000ms 且 ticks≈100 → TSC 校准窗口翻倍 (tick 频率真实 100Hz)
         *   win≈500ms 且 ticks≈50  → 一切正常 */
        if (win_us > 750000) {
            g_log->info("[apic] diag: TICK2X VERDICT: TSC calib window doubled (PIT mode0 ran ~20ms); tick rate nominal");
        } else if (ticks > 75) {
            g_log->info("[apic] diag: TICK2X VERDICT: PIT mode3 double-clocked on IOAPIC edge (~200Hz real)");
        } else {
            g_log->info("[apic] diag: TICK2X VERDICT: nominal (~100Hz, no 2x)");
        }
    }
    log_dec("[apic] route verify ticks=", ticks);

    if (ticks >= 2) {
        /* 成功: 重复投递证明钩子 EOI 正确清除 ISR 位, 路由链路完整 */
        g_log->info("[apic] ROUTE VERIFY OK — IOAPIC->LAPIC->IDT(vec 0x20)->handler->hook EOI chain proven");

        /* diag: 第二窗口持续投递证据 (排除窗口内偶发两次的弱证明) */
        if (g_cfg.apic_diag) {
            u32 before2 = g_tick_count;
            tsc_delay_ms(500);
            u32 ticks2 = g_tick_count - before2;
            log_dec("[apic] diag: sustain window ticks=", ticks2);
            if (ticks2 >= 2) {
                g_log->info("[apic] diag: SUSTAINED delivery confirmed (2nd window)");
            }
        }

        /* tick 线重新 mask (保留路由表, vector 保持 0x20), 避免后台周期中断;
         * PIT 恢复 mode 3 @ 18.2Hz (BIOS 惯例值, 已被 mask 不会投递)。
         * EOI 钩子保持注册 — legacy 线的持续投递依赖它。 */
        ioapic_program_gsi(info, tick_gsi, 0x20, tick_flags, 0);
        outb(0x43, 0x36);
        outb(0x40, 0x00);
        outb(0x40, 0x00);
        g_route_active = 1;
        g_log->info("[apic] APIC ROUTING ACTIVE (PIC masked; EOI hook live; tick line re-masked)");
    } else {
        /* 失败: 完整降级回 PIC (含注销 EOI 钩子) */
        g_log->error("[apic] ROUTE VERIFY FAILED — degrading to PIC routing");
        ioapic_mask_all(info);
        api->irq_register(0x20, (void *)0);
        api->register_apic_eoi(NULL);
        outb(0x43, 0x36);
        outb(0x40, 0x00);
        outb(0x40, 0x00);
        outb(0x21, imr1);
        io_wait();
        outb(0xA1, imr2);
        io_wait();
        g_log->info("[apic] PIC routing restored (IMR restored, IOAPIC masked, hook cleared)");
    }
}

/* ---- B7 diag: IRQ1/IRQ12 键鼠流观察窗 ---- *
 * 在 route switch 之后调用 (route 成功走 IOAPIC; 否则走 PIC IMR)。
 * 打锚点日志供测试脚本/人工在窗口内用 QEMU monitor sendkey/mouse_move
 * 注入输入; 窗口结束分别报告收包计数。计数 >1 即证明该线在当前路由
 * 模式下可持续投递 (一次按键=make+break 两包, 一次移动=多包)。 */
static void apic_diag_mouse_window(const struct dkm_kernel_api *api,
                                   struct madt_info *info) {
    g_log->info("[apic] diag: IRQ1/IRQ12 input stream check begin");

    /* 确保 TSC 延时可用 (route=0 时未校准: 现场校准并恢复 PIT 惯例状态) */
    if (!g_tsc_per_ms) {
        tsc_calibrate();
        outb(0x43, 0x36);
        outb(0x40, 0x00);
        outb(0x40, 0x00);
    }

    if (!api->irq_register) return;

    if (g_route_active) {
        /* IOAPIC 路由: 确保 IRQ12/IRQ1 重定向项放开 (legacy=1 时幂等重编) */
        u16 f12 = 0, f1 = 0;
        u32 gsi12 = isa_irq_to_gsi(12, &f12, info->isos, info->iso_count);
        u32 gsi1 = isa_irq_to_gsi(1, &f1, info->isos, info->iso_count);
        ioapic_program_gsi(info, gsi12, 0x2C, f12, 1);
        ioapic_program_gsi(info, gsi1, 0x21, f1, 1);
        g_log->info("[apic] diag: IRQ12+IRQ1 via IOAPIC redirection");
    } else {
        /* PIC 路由: 解 slave IMR bit4 (IRQ12); master IMR bit1 (IRQ1)
         * UTSM pic_remap 已留 0xFD (IRQ1 enable), 幂等确认。
         * diag 属实验开关, 显式改变 PIC 状态并打日志; UTSM PIC 分支
         * 会做 8259 EOI, diag handler 返回 0 即可。 */
        outb(0xA1, (u8)(inb(0xA1) & (u8)~0x10u));
        outb(0x21, (u8)(inb(0x21) & (u8)~0x02u));
        g_log->info("[apic] diag: IRQ12+IRQ1 via PIC (IMR bits cleared)");
    }

    /* 控制器交互必须全轮询完成: 期间到达的 IRQ (KBD bit0 一直开着)
     * 因 handler 未注册只会被 EOI 丢弃, 不会偷走命令响应字节。
     * (实测教训: 先注册 handler 时, 0x20 读 cfg 的响应被 kbd handler
     * 当扫描码读走, 轮询侧 cfg read timeout) */
    ps2_aux_enable_stream();

    /* 交互全部完成, 现在才注册诊断 handler */
    if (api->irq_register(0x2C, (void *)(uintptr_t)apic_mouse_diag_handler) != 0) {
        g_log->warn("[apic] diag: irq_register(0x2C) failed");
        return;
    }
    if (api->irq_register(0x21, (void *)(uintptr_t)apic_kbd_diag_handler) != 0) {
        g_log->warn("[apic] diag: irq_register(0x21) failed");
        return;
    }

    /* 6000ms 窗口: 注入方需经 "串口日志落盘(块缓冲) → 测试脚本轮询" 的
     * 可见性延迟 (~1s 量级), 3s 窗口实测会被注入延迟吃光 */
    g_log->info("[apic] diag: INPUT OBSERVE WINDOW 6000ms — sendkey/mouse_move now");
    u32 m_before = g_mouse_pkt_count;
    u32 k_before = g_kbd_pkt_count;
    tsc_delay_ms(6000);
    u32 mpkts = g_mouse_pkt_count - m_before;
    u32 kpkts = g_kbd_pkt_count - k_before;
    log_dec("[apic] diag: mouse packets observed=", mpkts);
    log_dec("[apic] diag: kbd packets observed=", kpkts);
    if (mpkts > 1) {
        g_log->info("[apic] diag: IRQ12 SUSTAINED delivery confirmed");
    } else if (mpkts == 1) {
        g_log->warn("[apic] diag: IRQ12 single packet only (check EOI path)");
    } else {
        g_log->info("[apic] diag: no mouse packets (inject via monitor mouse_move)");
    }
    if (kpkts > 1) {
        g_log->info("[apic] diag: IRQ1 SUSTAINED delivery confirmed");
    } else if (kpkts == 1) {
        g_log->warn("[apic] diag: IRQ1 single packet only (check EOI path)");
    } else {
        g_log->info("[apic] diag: no kbd packets (inject via monitor sendkey)");
    }
    /* handler 保持注册: stage3 ps2kbd 经 irq_register(1) 覆盖键盘 handler,
     * DSK mouseInit 经 irq_register(12) 覆盖鼠标 handler, 均无冲突;
     * 重定向项/IMR 保持现状 (ps2kbd/mouseInit 有完整初始化序列)。 */
}

__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    g_log->info("[apic] init begin");

    if (!api->irq_register) {
        g_log->error("[apic] kernel irq_register missing");
        return -1;
    }
    g_log->info("[apic] kernel IRQ backend present: PIC");

    /* B7: FUCK 配置自解析 (lapic_enable/ioapic_init/apic_route/apic_route_legacy/apic_diag) */
    load_fuck_config(api);

    if (!cpu_has_lapic()) {
        g_log->warn("[apic] CPUID local APIC not present; PIC fallback active");
        g_log->info("[apic] driver ready");
        return 0;
    }
    g_log->info("[apic] CPUID local APIC present");

    u64 apic_base = rdmsr(0x1B);
    u64 lapic_phys = apic_base & 0xFFFFF000ULL;
    log_hex("[apic] IA32_APIC_BASE=", apic_base);
    log_hex("[apic] LAPIC phys base=", lapic_phys);
    if (apic_base & (1ULL << 8)) g_log->info("[apic] bootstrap processor");
    if (apic_base & (1ULL << 11)) g_log->info("[apic] LAPIC enabled by firmware");

    struct madt_info info;
    info.lapic_phys = lapic_phys;
    info.flags = 0;
    info.lapic_count = 0;
    info.ioapic_count = 0;
    info.iso_count = 0;

    const acpi_madt *madt = find_madt(api->rsdp_address);
    if (madt) {
        g_log->info("[apic] MADT found");
        info.lapic_phys = (u64)madt->lapic_addr;
        info.flags = madt->flags;
        log_hex("[apic] MADT lapic_phys=", info.lapic_phys);
        log_hex("[apic] MADT flags=", info.flags);
        parse_madt_entries(madt, &info);
        log_hex("[apic] CPU LAPIC count=", info.lapic_count);
        log_hex("[apic] IOAPIC count=", info.ioapic_count);
        log_hex("[apic] ISO count=", info.iso_count);
    } else {
        g_log->warn("[apic] MADT not found; PIC fallback active");
    }

    if (api->hhdm_offset && info.lapic_phys) {
        g_lapic = (volatile u32 *)(uintptr_t)(api->hhdm_offset + info.lapic_phys);
        g_lapic_id = g_lapic[0x20 / 4] >> 24;
        log_hex("[apic] LAPIC ID reg=", g_lapic[0x20 / 4]);
        log_hex("[apic] LAPIC version reg=", g_lapic[0x30 / 4]);
        log_hex("[apic] LAPIC SVR reg=", g_lapic[0xF0 / 4]);
        log_hex("[apic] LAPIC TPR reg=", g_lapic[0x80 / 4]);
        log_hex("[apic] BSP LAPIC ID=", g_lapic_id);
    } else {
        g_log->warn("[apic] HHDM unavailable; skip LAPIC MMIO read");
        g_lapic = (volatile u32 *)0;
    }

    /* ---- LAPIC 使能（真机必需，QEMU 也可安全执行） ---- */
    if (g_lapic && g_cfg.lapic_enable) {
        lapic_enable();
    } else if (!g_cfg.lapic_enable) {
        g_log->info("[apic] lapic_enable=0: skip LAPIC software enable");
    }

    /* ---- IOAPIC 初始化（真机必需，QEMU 也可安全执行） ---- */
    if (g_cfg.ioapic_init) {
        for (u32 i = 0; i < info.ioapic_count; i++) {
            g_log->info("[apic] initializing IOAPIC");
            log_hex("[apic]   index=", i);
            ioapic_init_one(&info.ioapics[i], api->hhdm_offset,
                             info.isos, info.iso_count,
                             api->mm_map_mmio);
        }
    } else {
        g_log->info("[apic] ioapic_init=0: skip IOAPIC programming (PIC mode)");
    }

    /* ---- B7: 路由接管 (默认关, FUCK apic_route=1 启用) ---- */
    if (g_cfg.apic_route && g_cfg.ioapic_init) {
        apic_route_switch(api, &info);
    } else {
        g_log->info("[apic] apic_route=0: PIC routing retained (default)");
    }

    /* ---- B7 diag: IRQ12 鼠标流观察窗 (apic_diag=1, 两种路由模式皆可测) ---- */
    if (g_cfg.apic_diag) {
        apic_diag_mouse_window(api, &info);
    }

    /* 重定向表回读 (验证证据; 无 IOAPIC 时无输出) */
    if (g_cfg.ioapic_init) {
        ioapic_dump_table(&info);
    }

    if (g_route_active) {
        g_log->info("[apic] discovery + init complete; APIC routing active");
    } else {
        g_log->info("[apic] discovery + init complete; PIC routing retained as fallback");
    }
    g_log->info("[apic] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
