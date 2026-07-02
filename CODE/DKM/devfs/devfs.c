/* DKM DevFS Driver — virtual device filesystem
 * Stage 2, optional, depends on "vfs", provides "devfs".
 * Exposes kernel platform info as virtual files under /dev/.
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
static const char *const g_depends[] = { "vfs" };
static const char *const g_provides[] = { "devfs" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "devfs",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 7,
    .stage          = 2,
    .flags          = 0,
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
    char buf[19]; u32 p = 0;
    buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 15; i >= 0; i--) buf[p++] = h[(val>>(i*4))&0xf];
    buf[p] = 0;
    g_log->info(prefix); g_log->info(buf);
}

/* ---------------------------------------------------------------
 * Virtual file helpers
 * --------------------------------------------------------------- */
static void devfs_file(const char *name, const char *content) {
    g_log->info("[devfs] /dev/");
    g_log->info(name);
    g_log->info("[devfs]   ");
    g_log->info(content);
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

    g_log->info("[devfs] virtual device filesystem");

    /* /dev/version */
    devfs_file("version", "Deshab kernel 0.1.0");

    /* /dev/platform */
    devfs_file("platform", "x86_64 SAS-R0");

    /* /dev/fb0 */
    if (api->fb_address) {
        char info[64];
        info[0] = 0;
        /* manual number conversion for width */
        {
            u64 v = api->fb_width;
            char tmp[24]; u32 p = 0;
            if (v == 0) tmp[p++] = '0';
            else { char r[24]; u32 q = 0; while (v) { r[q++] = '0' + (char)(v%10); v/=10; } while (q) tmp[p++] = r[--q]; }
            tmp[p++] = 'x';
            v = api->fb_height;
            if (v == 0) tmp[p++] = '0';
            else { char r[24]; u32 q = 0; while (v) { r[q++] = '0' + (char)(v%10); v/=10; } while (q) tmp[p++] = r[--q]; }
            tmp[p++] = 'x';
            v = api->fb_bpp;
            if (v == 0) tmp[p++] = '0';
            else { char r[24]; u32 q = 0; while (v) { r[q++] = '0' + (char)(v%10); v/=10; } while (q) tmp[p++] = r[--q]; }
            tmp[p++] = ' ';
            tmp[p++] = 'f';
            tmp[p++] = 'b';
            tmp[p++] = '0';
            tmp[p] = 0;
            for (u32 i = 0; i <= p; i++) info[i] = tmp[i];
        }
        devfs_file("fb0", info);
    }

    /* /dev/modules — count from boot_modules */
    const struct limine_module_response *rsp =
        (const struct limine_module_response *)api->boot_modules_response;
    if (rsp) {
        char buf[24];
        buf[0] = '0'; buf[1] = 'x';
        u64 v = rsp->module_count;
        u32 p = 2;
        if (v == 0) buf[p++] = '0';
        else {
            char r[18]; u32 q = 0;
            while (v) { r[q++] = "0123456789abcdef"[v&0xf]; v>>=4; }
            while (q) buf[p++] = r[--q];
        }
        buf[p] = 0;
        devfs_file("modules", buf);

        /* /dev/boot — list paths */
        g_log->info("[devfs] /dev/boot/");
        for (u64 i = 0; i < rsp->module_count; i++) {
            struct limine_file *f = rsp->modules[i];
            if (f && f->path) {
                g_log->info("[devfs]   ");
                g_log->info(f->path);
            }
        }
    }

    g_log->info("[devfs] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
