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
