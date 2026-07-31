/* DKM ACPI Driver - RSDP scan / RSDT parse / table enumeration
 * Stage 0, optional, provides "acpi".
 * Scans BIOS memory for RSDP, then walks RSDT/XSDT to list all ACPI tables.
 */

#include <stdint.h>

/* ---------------------------------------------------------------
 * DKM ABI types
 * --------------------------------------------------------------- */
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

/* ---------------------------------------------------------------
 * ACPI structures
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;
} acpi_rsdp_v1;

typedef struct __attribute__((packed)) {
    /* RSDP v1 fields */
    char     signature[8];
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;
    /* RSDP v2+ fields */
    u32      length;
    u64      xsdt_addr;
    u8       ext_checksum;
    u8       reserved[3];
} acpi_rsdp_v2;

typedef struct __attribute__((packed)) {
    char     signature[4];
    u32      length;
    u8       revision;
    u8       checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    u32      oem_revision;
    u32      creator_id;
    u32      creator_revision;
} acpi_sdt_header;

/* ACPI Generic Address Structure (GAS) — HPET 等表使用 */
typedef struct __attribute__((packed)) {
    u8  space_id;     /* 0=SystemMemory, 1=SystemIO, 2=PCIConfig, ... */
    u8  bit_width;
    u8  bit_offset;
    u8  access_size;  /* 0=undefined, 1=byte, 2=word, 3=dword, 4=qword */
    u64 address;
} acpi_generic_address;

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_provides[] = { "acpi" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "acpi",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 1,   /* DKM_CLASS_PLATFORM */
    .stage          = 0,
    .flags          = 0,   /* optional */
    .priority       = 0,
    .depends        = NULL,
    .depends_count  = 0,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;

static u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }

static int memneq(const char *a, const char *b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
    }
    return 0;
}

static u8 checksum8(const u8 *data, u32 len) {
    u8 sum = 0;
    for (u32 i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum;
}

static void log_hex(const char *prefix, u64 val) {
    static const char hex[] = "0123456789abcdef";
    char buf[32];
    u32 pos = 0;
    while (pos < 30) { buf[pos] = 0; pos++; }
    pos = 0;
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;

    /* manual concat via serial_write-like approach: pass prefix then buf */
    g_log->info(prefix);
    g_log->info(buf);
}

/* ---------------------------------------------------------------
 * RSDP scan
 * --------------------------------------------------------------- */
static const acpi_rsdp_v2 *acpi_find_rsdp(const void *limine_rsdp) {
    /* If Limine gave us the RSDP, use it directly */
    if (limine_rsdp) {
        const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)limine_rsdp;
        if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (checksum8((const u8 *)rsdp, 20) == 0) {
                return rsdp;
            }
        }
    }
    /* EBDA: extended BIOS data area, base at 0x40E */
    {
        u16 ebda_seg = *(const volatile u16 *)(uintptr_t)0x40E;
        u64 ebda_base = (u64)ebda_seg << 4;
        if (ebda_base >= 0x80000ULL && ebda_base < 0x100000ULL) {
            for (u64 off = 0; off < 1024; off += 16) {
                const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)(uintptr_t)(ebda_base + off);
                if (off + sizeof(acpi_rsdp_v1) > 1024) break;
                if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
                    if (checksum8((const u8 *)rsdp, 20) == 0) {
                        return rsdp;
                    }
                }
            }
        }
    }

    /* BIOS read-only area: 0x000E0000 - 0x000FFFFF */
    for (u64 addr = 0x000E0000ULL; addr < 0x00100000ULL; addr += 16) {
        const acpi_rsdp_v2 *rsdp = (const acpi_rsdp_v2 *)(uintptr_t)addr;
        if (memneq(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (checksum8((const u8 *)rsdp, 20) == 0) {
                return rsdp;
            }
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------
 * MCFG (PCIe ECAM Memory-mapped Configuration) 解析
 *
 * **QEMU 无法测试但真机必需**: PCIe 配置空间通过 ECAM 访问,
 * MCFG 表提供 ECAM 基址和总线范围。没有 MCFG 解析,
 * 真机无法访问 Bus 0 以外的 PCIe 设备配置空间。
 * QEMU 的 PCI 设备都在 Bus 0, 使用传统 CF8/CFC 端口即可。
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    u64 base_address;       /* ECAM 基址 (4K 对齐, 256MB per bus) */
    u16 pci_segment_group;  /* PCI 段组号 (通常为 0) */
    u8  start_bus_number;   /* 起始总线号 */
    u8  end_bus_number;     /* 结束总线号 */
    u32 reserved;
} mcfg_entry;

typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u64 reserved;           /* 保留, 通常为 0 */
    /* mcfg_entry entries[] 紧跟 */
} mcfg_table;

static void acpi_parse_mcfg(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(mcfg_table)) return;

    const mcfg_table *mcfg = (const mcfg_table *)hdr;
    u32 entry_size = sizeof(mcfg_entry);
    u32 entries_len = mcfg->hdr.length - sizeof(mcfg_table);
    u32 count = entries_len / entry_size;

    g_log->info("[acpi] MCFG entries:");
    log_hex("[acpi]   count=", count);

    for (u32 i = 0; i < count; i++) {
        const mcfg_entry *e = (const mcfg_entry *)((const u8 *)mcfg + sizeof(mcfg_table) + i * entry_size);
        log_hex("[acpi]   MCFG base=", e->base_address);
        log_hex("[acpi]   MCFG seg=", e->pci_segment_group);
        log_hex("[acpi]   MCFG bus_start=", e->start_bus_number);
        log_hex("[acpi]   MCFG bus_end=", e->end_bus_number);

        /* 验证 ECAM 基址合理性 */
        if (e->base_address == 0 || e->base_address == 0xFFFFFFFFFFFFFFFFULL) {
            g_log->warn("[acpi]   MCFG entry has invalid base address");
            continue;
        }
        /* 真机: ECAM 基址通常在 0xB0000000 - 0xBFFFFFFF (3G-4G 范围),
         * 但也可能在 4G 以上 (特别是大内存系统) */
        if (e->base_address >= 0x100000000ULL) {
            g_log->info("[acpi]   MCFG base above 4G — requires MMIO mapping");
        }
    }
}

/* ---------------------------------------------------------------
 * HPET (High Precision Event Timer) 解析
 *
 * 真机需要 HPET 替代 PIT 作为高精度定时器源。
 * QEMU 默认也提供 HPET, 但当前内核使用 PIT 即可。
 * HPET 信息留作后续高精度定时器实现的参考。
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    u8  hardware_rev_id;
    u8  comparator_count : 5;
    u8  counter_size : 1;       /* 0=32-bit, 1=64-bit */
    u8  reserved0 : 1;
    u8  legacy_replacement : 1; /* 1=LegacyReplacement IRQ routing available */
    u16 pci_vendor_id;
    acpi_generic_address address;  /* HPET 寄存器基址 */
    u8  hpet_number;
    u16 minimum_tick;
    u8  page_protection;
} hpet_entry_data;

/* HPET 表不含子条目, 只有 1 个 data block */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 event_timer_block_id;  /* 与 hpet_entry_data 的前 4 字节相同 */
    acpi_generic_address base_address;
    u8  hpet_number;
    u16 minimum_tick;
    u8  page_protection;
} hpet_table;

static void acpi_parse_hpet(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(hpet_table)) return;

    const hpet_table *hpet = (const hpet_table *)hdr;

    u32 block_id = hpet->event_timer_block_id;
    u8  comp_count = (block_id >> 8) & 0x1f;
    u8  count_size = (block_id >> 13) & 1;
    u8  legacy     = (block_id >> 15) & 1;
    u8  rev_id     = block_id & 0xff;
    u16 vendor_id  = (block_id >> 16) & 0xffff;

    g_log->info("[acpi] HPET info:");
    log_hex("[acpi]   rev_id=", rev_id);
    log_hex("[acpi]   comparators=", comp_count);
    log_hex("[acpi]   counter_size=", count_size ? 64 : 32);
    log_hex("[acpi]   legacy_irq=", legacy);
    log_hex("[acpi]   vendor_id=", vendor_id);
    log_hex("[acpi]   min_tick=", hpet->minimum_tick);

    /* HPET 地址空间类型 */
    if (hpet->base_address.space_id == 0) {
        /* System Memory — MMIO 访问 */
        u64 addr = hpet->base_address.address;
        log_hex("[acpi]   HPET MMIO addr=", addr);
        if (addr >= 0x100000000ULL) {
            g_log->info("[acpi]   HPET base above 4G — requires MMIO mapping");
        }
    } else if (hpet->base_address.space_id == 1) {
        /* System I/O — 端口 I/O */
        log_hex("[acpi]   HPET I/O port=", hpet->base_address.address);
    }
}

/* ---------------------------------------------------------------
 * MADT (APIC) 解析 — 为 apic 驱动提供数据
 * --------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    acpi_sdt_header hdr;
    u32 lapic_addr;
    u32 flags;
} acpi_madt;

static void acpi_parse_madt(const acpi_sdt_header *hdr) {
    if (!hdr || hdr->length < sizeof(acpi_madt)) return;
    const acpi_madt *madt = (const acpi_madt *)hdr;

    g_log->info("[acpi] MADT detail:");
    log_hex("[acpi]   LAPIC addr=", madt->lapic_addr);
    log_hex("[acpi]   flags=", madt->flags);
    if (madt->flags & 1) {
        g_log->info("[acpi]   PCAT_COMPAT=1 (8259 PIC present)");
    }

    /* 枚举 MADT 子条目做汇总统计 */
    const u8 *base = (const u8 *)madt;
    u32 off = sizeof(acpi_madt);
    u32 lapic_count = 0, ioapic_count = 0, iso_count = 0, nmi_count = 0;

    while (off + 2 <= madt->hdr.length) {
        const u8 *entry = base + off;
        u8 type = entry[0];
        u8 len = entry[1];
        if (len < 2 || off + len > madt->hdr.length) break;

        if (type == 0) lapic_count++;
        else if (type == 1) ioapic_count++;
        else if (type == 2) iso_count++;
        else if (type == 3 || type == 4) nmi_count++;

        off += len;
    }

    log_hex("[acpi]   LAPIC entries=", lapic_count);
    log_hex("[acpi]   IOAPIC entries=", ioapic_count);
    log_hex("[acpi]   ISO entries=", iso_count);
    log_hex("[acpi]   NMI entries=", nmi_count);
}

/* ---------------------------------------------------------------
 * Table enumeration
 * --------------------------------------------------------------- */
static void acpi_enumerate_rsdt(const acpi_sdt_header *rsdt, u32 entry_size) {
    u32 entry_count = (rsdt->length - sizeof(acpi_sdt_header)) / entry_size;
    const u8 *entries = (const u8 *)rsdt + sizeof(acpi_sdt_header);

    g_log->info("[acpi] tables found");
    for (u32 i = 0; i < entry_count; i++) {
        u64 addr;
        if (entry_size == 4) {
            addr = (u64)((const u32 *)entries)[i];
        } else {
            addr = ((const u64 *)entries)[i];
        }
        if (addr == 0) continue;

        const acpi_sdt_header *hdr = (const acpi_sdt_header *)(uintptr_t)addr;
        char sig[5];
        sig[0] = hdr->signature[0];
        sig[1] = hdr->signature[1];
        sig[2] = hdr->signature[2];
        sig[3] = hdr->signature[3];
        sig[4] = 0;

        g_log->info("[acpi]   ");
        g_log->info(sig);
        log_hex("[acpi]   addr=", addr);
        log_hex("[acpi]   len =", (u64)hdr->length);

        /* 对关键表做详细解析 */
        if (sig[0]=='A' && sig[1]=='P' && sig[2]=='I' && sig[3]=='C') {
            acpi_parse_madt(hdr);
        } else if (sig[0]=='M' && sig[1]=='C' && sig[2]=='F' && sig[3]=='G') {
            acpi_parse_mcfg(hdr);
        } else if (sig[0]=='H' && sig[1]=='P' && sig[2]=='E' && sig[3]=='T') {
            acpi_parse_hpet(hdr);
        }
    }
}

/* ---------------------------------------------------------------
 * Driver entry points
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) {
        return -1;
    }
    g_log = api->log;

    g_log->info("[acpi] scanning for RSDP");

    const acpi_rsdp_v2 *rsdp = acpi_find_rsdp(api->rsdp_address);
    if (!rsdp) {
        g_log->error("[acpi] RSDP not found");
        return 0;  /* optional driver, don't fail boot */
    }

    g_log->info("[acpi] RSDP found");
    log_hex("[acpi] revision =", (u64)rsdp->revision);
    g_log->info("[acpi] oem_id");
    {
        char oem[7];
        for (int i = 0; i < 6; i++) oem[i] = rsdp->oem_id[i];
        oem[6] = 0;
        g_log->info(oem);
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_addr) {
        g_log->info("[acpi] using XSDT");
        const acpi_sdt_header *xsdt = (const acpi_sdt_header *)(uintptr_t)rsdp->xsdt_addr;
        acpi_enumerate_rsdt(xsdt, 8);
    } else if (rsdp->rsdt_addr) {
        g_log->info("[acpi] using RSDT");
        const acpi_sdt_header *rsdt = (const acpi_sdt_header *)(uintptr_t)rsdp->rsdt_addr;
        acpi_enumerate_rsdt(rsdt, 4);
    } else {
        g_log->warn("[acpi] no RSDT/XSDT address");
    }

    g_log->info("[acpi] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
