/* DKM APIC Driver — APIC discovery + IOAPIC init + LAPIC enable
 * Stage 0, required, depends on "timer", provides "irq" and "apic".
 *
 * 真机路径:
 *   1. LAPIC 使能 + Spurious Interrupt Vector 注册
 *   2. IOAPIC 重定向表初始化: 逐条目编程, 将 ISA IRQ 映射到 vector 0x20-0x2F
 *      (兼容 PIC 布局), 逐条目 mask
 *   3. MADT ISO (Interrupt Source Override) 处理: 按 ACPI 规范修正 IRQ→GSI 映射
 *   4. SIPI 多核启动框架 (预留, 未实现)
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
    .version        = "0.1.0",
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

static const struct dkm_log_api *g_log;

static __inline__ void apic_mb(void) {
    __asm__ volatile("" ::: "memory");
}

static int memeq(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
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

static int cpu_has_lapic(void) {
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    (void)a;
    (void)b;
    (void)c;
    return (d & (1u << 9)) != 0;
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

/* IOAPIC 重定向表项 (64-bit, 分两次 32-bit 读写)
 *   bits 0-7:   interrupt vector (0x20-0xFF)
 *   bit  8:     delivery mode: 0=fixed, 1=lowest, 2=SMI, 4=NMI, 5=INIT, 7=ExtINT
 *   bit  9:     destination mode: 0=physical, 1=logical
 *   bit  10:    delivery status (RO)
 *   bit  11:    polarity: 0=active high, 1=active low
 *   bit  12:    remote IRR (RO, level-triggered only)
 *   bit  13:    trigger mode: 0=edge, 1=level
 *   bit  14:    mask: 0=enabled, 1=masked
 *   bit  15:    reserved
 *   bits 16-55: reserved
 *   bits 56-63: destination APIC ID (physical mode) or MDA (logical mode)
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

/* ---- IOAPIC 初始化 ---- *
 * 逐条目编程重定向表: ISA IRQ→vector 映射 + mask all。
 * 真机关键: 处理 MADT ISO (IRQ0→GSI2 是最常见的 override)。
 * PIC 兼容模式: vector = 0x20 + IRQ (与 PIC remap 布局一致)。
 *
 * **QEMU 无法测试但真机必需的代码路径**:
 *   1. ISO 处理: 真机几乎总有 IRQ0→GSI2 override, QEMU 默认无 override
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

            entry = (u64)vec;          /* vector */
            /* delivery mode = fixed (0) */
            /* destination mode = physical (0) */

            /* 极性: bit 11. ISA 默认 active-high (0).
             * ISO flags bit 1: 0=conforms (active-high for ISA), 1=active-low */
            if (flags & 0x03) {
                if (flags & 0x02) entry |= (1ULL << 11);  /* active low */
            }

            /* 触发模式: bit 13. ISA 默认 edge (0).
             * ISO flags bit 3: 0=conforms (edge for ISA), 1=level */
            if (flags & 0x08) {
                if (flags & 0x04) entry |= (1ULL << 13);  /* level triggered */
            }

            /* destination: BSP APIC ID (读自 LAPIC) */
            /* 暂时硬编码 destination = 0 (BSP), 后续从 LAPIC ID 寄存器获取 */
            entry |= (0ULL << 56);  /* destination APIC ID = 0 */
        }

        entry |= (1ULL << 14);  /* mask = 1, 所有条目默认屏蔽 */

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

static void wrmsr(u32 msr, u64 val) {
    u32 lo = (u32)(val & 0xffffffffu);
    u32 hi = (u32)(val >> 32);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

static void lapic_enable(u64 hhdm_offset, u64 lapic_phys) {
    volatile u32 *lapic = (volatile u32 *)(uintptr_t)(hhdm_offset + lapic_phys);

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
        volatile u32 *lapic = (volatile u32 *)(uintptr_t)(api->hhdm_offset + info.lapic_phys);
        log_hex("[apic] LAPIC ID reg=", lapic[0x20 / 4]);
        log_hex("[apic] LAPIC version reg=", lapic[0x30 / 4]);
        log_hex("[apic] LAPIC SVR reg=", lapic[0xF0 / 4]);
    } else {
        g_log->warn("[apic] HHDM unavailable; skip LAPIC MMIO read");
    }

    /* ---- LAPIC 使能（真机必需，QEMU 也可安全执行） ---- */
    if (api->hhdm_offset && info.lapic_phys) {
        lapic_enable(api->hhdm_offset, info.lapic_phys);
    }

    /* ---- IOAPIC 初始化（真机必需，QEMU 也可安全执行） ---- */
    for (u32 i = 0; i < info.ioapic_count; i++) {
        g_log->info("[apic] initializing IOAPIC");
        log_hex("[apic]   index=", i);
        ioapic_init_one(&info.ioapics[i], api->hhdm_offset,
                         info.isos, info.iso_count,
                         api->mm_map_mmio);
    }

    g_log->info("[apic] discovery + init complete; PIC routing retained as fallback");
    g_log->info("[apic] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
