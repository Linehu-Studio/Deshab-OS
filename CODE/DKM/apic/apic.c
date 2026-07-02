/* DKM APIC Driver — APIC discovery with PIC fallback
 * Stage 0, required, depends on "timer", provides "irq" and "apic".
 * Current kernel IRQ backend is still PIC; this driver only discovers APIC
 * topology and keeps PIC routing active.
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

struct madt_info {
    u64 lapic_phys;
    u32 flags;
    u32 lapic_count;
    u32 ioapic_count;
    u32 iso_count;
};

static const struct dkm_log_api *g_log;

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
            u8 apic_id = entry[3];
            u32 flags = *(const u32 *)(const void *)(entry + 4);
            if (flags & 1u) info->lapic_count++;
            log_hex("[apic] CPU LAPIC id=", apic_id);
        } else if (type == 1 && len >= 12) {
            u8 ioapic_id = entry[2];
            u32 addr = *(const u32 *)(const void *)(entry + 4);
            u32 gsi = *(const u32 *)(const void *)(entry + 8);
            info->ioapic_count++;
            log_hex("[apic] IOAPIC id=", ioapic_id);
            log_hex("[apic] IOAPIC addr=", addr);
            log_hex("[apic] IOAPIC gsi_base=", gsi);
        } else if (type == 2 && len >= 10) {
            u8 source = entry[3];
            u32 gsi = *(const u32 *)(const void *)(entry + 4);
            u16 flags = *(const u16 *)(const void *)(entry + 8);
            info->iso_count++;
            log_hex("[apic] ISO source=", source);
            log_hex("[apic] ISO gsi=", gsi);
            log_hex("[apic] ISO flags=", flags);
        } else if (type == 5 && len >= 12) {
            u64 override = *(const u64 *)(const void *)(entry + 4);
            info->lapic_phys = override;
            log_hex("[apic] LAPIC override=", override);
        }

        off += len;
    }
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

    g_log->info("[apic] discovery complete; PIC routing retained");
    g_log->info("[apic] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
