/* DKM BootFS Driver — exposes Limine boot modules as a simple filesystem
 * Stage 1, required, provides "bootfs".
 * Lists all boot modules with path, size, and address.
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
    const void *pci_api;
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

/* minimal limine types matching kernel */
struct limine_file {
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

struct limine_module_response {
    u64 revision;
    u64 module_count;
    struct limine_file **modules;
};

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_provides[] = { "bootfs" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "bootfs",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 7,   /* DKM_CLASS_FS */
    .stage          = 1,
    .flags          = 1,   /* DKM_F_REQUIRED */
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
 * Log helpers
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;

static void log_hex(const char *prefix, u64 val) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    u32 pos = 0;
    buf[pos++] = '0'; buf[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xf];
    }
    buf[pos] = 0;
    g_log->info(prefix);
    g_log->info(buf);
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) return -1;
    g_log = api->log;

    const struct limine_module_response *rsp =
        (const struct limine_module_response *)api->boot_modules_response;

    if (!rsp) {
        g_log->error("[bootfs] no boot module response");
        return -1;
    }

    g_log->info("[bootfs] boot module filesystem");
    log_hex("[bootfs] file count=", rsp->module_count);

    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (!f) continue;

        g_log->info("[bootfs] ----------");
        if (f->path) {
            g_log->info("[bootfs] file");
            g_log->info(f->path);
        }
        log_hex("[bootfs] size=", f->size);
        log_hex("[bootfs] addr=", (u64)(uintptr_t)f->address);
        if (f->cmdline) {
            g_log->info("[bootfs] cmd");
            g_log->info(f->cmdline);
        }
    }

    g_log->info("[bootfs] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
