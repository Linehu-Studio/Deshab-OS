/* DKM VFS Driver — Virtual File System layer
 * Stage 2, required, depends on "bootfs", provides "vfs".
 * Mounts bootfs modules as files, implements basic open/read.
 * Demo: reads and displays first 64 bytes of manifest.json.
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
    const void *vfs_api;
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

struct limine_file {
    u64 revision;
    void *address;
    u64 size;
    char *path;
    char *cmdline;
    u8 _pad[56];
};

struct limine_module_response {
    u64 revision;
    u64 module_count;
    struct limine_file **modules;
};

/* ---------------------------------------------------------------
 * Driver descriptor
 * --------------------------------------------------------------- */
static const char *const g_depends[] = { "bootfs" };
static const char *const g_provides[] = { "vfs" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "vfs",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 8,   /* DKM_CLASS_FS_CORE */
    .stage          = 2,
    .flags          = 1,
    .priority       = 0,
    .depends        = g_depends,
    .depends_count  = 1,
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

static void log_hex(const char *prefix, u64 val) {
    static const char h[] = "0123456789abcdef";
    char buf[19];
    u32 p = 0;
    buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 15; i >= 0; i--) buf[p++] = h[(val >> (i*4)) & 0xf];
    buf[p] = 0;
    g_log->info(prefix); g_log->info(buf);
}

static u64 min64(u64 a, u64 b) { return a < b ? a : b; }

static int path_match(const char *bp, const char *mp) {
    /* strip leading '/' from bootloader path */
    while (*bp == '/') bp++;
    u64 i = 0;
    while (bp[i] && mp[i] && bp[i] == mp[i]) i++;
    return bp[i] == 0 && mp[i] == 0;
}

static int str_len(const char *s) {
    int l = 0; while (s[l]) l++; return l;
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

    if (!rsp) { g_log->error("[vfs] no modules"); return -1; }

    g_log->info("[vfs] mounting bootfs as root");
    log_hex("[vfs] files=", rsp->module_count);

    /* list all files */
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (!f || !f->path) continue;
        g_log->info("[vfs]  /");
        g_log->info(f->path);
    }

    /* ---- demo: open & read /driver/manifest.json ---- */
    g_log->info("[vfs] demo: open //driver/manifest.json");

    struct limine_file *target = NULL;
    for (u64 i = 0; i < rsp->module_count; i++) {
        struct limine_file *f = rsp->modules[i];
        if (f && f->path && path_match(f->path, "driver/manifest.json")) {
            target = f; break;
        }
    }

    if (!target) {
        g_log->warn("[vfs] manifest.json not found");
    } else {
        log_hex("[vfs] size=", target->size);
        g_log->info("[vfs] content (first 64 bytes):");

        /* dump first 64 bytes as hex + ascii */
        u64 n = min64(64, target->size);
        const u8 *data = (const u8 *)target->address;
        char line[80];
        for (u64 off = 0; off < n; off += 16) {
            u32 pos = 0;
            for (u32 j = 0; j < 16 && off + j < n; j++) {
                u8 b = data[off + j];
                line[pos++] = "0123456789abcdef"[b >> 4];
                line[pos++] = "0123456789abcdef"[b & 0xf];
                line[pos++] = ' ';
            }
            /* ascii */
            line[pos++] = ' ';
            for (u32 j = 0; j < 16 && off + j < n; j++) {
                u8 c = data[off + j];
                line[pos++] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            line[pos] = 0;
            g_log->info(line);
        }
    }

    g_log->info("[vfs] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
