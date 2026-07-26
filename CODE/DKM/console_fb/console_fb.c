/* DKM Console Framebuffer Driver
 * Stage 1, optional, provides "console".
 * Clears the framebuffer to a solid light-blue background so the screen is
 * clean before DSK takes over and draws the Logo. The former 4-color arc
 * loading ring has been removed — DSK now shows a static Logo instead.
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
    u32 version; u32 size; u64 feature_bits;
    const struct dkm_log_api *log;
    const void *mem, *utsm, *irq, *pci_api, *dma, *vfs, *net, *timer, *drr;
    const void *rsdp_address, *fb_address;
    u64 fb_width, fb_height, fb_pitch; u16 fbpp;
};

struct dkm_driver_handle;

struct dkm_driver_desc {
    u32 magic; u16 abi_version; u16 desc_size;
    const char *name, *version, *vendor;
    u32 driver_class, stage, flags, priority;
    const char *const *depends; u32 depends_count;
    const char *const *provides; u32 provides_count;
    u64 min_kernel_abi, feature_bits, reserved0, reserved1;
};

static const char *const g_provides[] = { "console" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic=DKM_DRIVER_MAGIC, .abi_version=DKM_ABI_VERSION,
    .desc_size=sizeof(struct dkm_driver_desc),
    .name="console_fb", .version="0.3.0", .vendor="Deshab",
    .driver_class=5, .stage=1, .flags=0, .priority=0,
    .depends=NULL, .depends_count=0,
    .provides=g_provides, .provides_count=1,
    .min_kernel_abi=1,
};

/* ---------------------------------------------------------------
 * Colors
 * --------------------------------------------------------------- */
static const u32 BG = 0xFFC8E0F0; /* light blue background — cleared before DSK Logo */

/* ---------------------------------------------------------------
 * Drawing helpers – all integer arithmetic, no soft-float
 * --------------------------------------------------------------- */
static void fb_fill(u32 *fb, u64 pitch, u64 w, u64 h, u32 color) {
    for (u64 y = 0; y < h; y++) {
        u32 *line = (u32 *)((u8 *)fb + y * pitch);
        for (u64 x = 0; x < w; x++) line[x] = color;
    }
}

/* ---------------------------------------------------------------
 * Driver entry
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;
    if (!api || !api->log) return -1;
    if (!api->fb_address || !api->fb_width) {
        api->log->warn("[console_fb] no framebuffer");
        return 0;
    }

    api->log->info("[console_fb] clearing boot screen");

    /* Solid background fill only — DSK draws the Logo next. Clearing here
     * avoids firmware/UEFI leftover garbage being visible during the
     * UTSM→DSK handoff. */
    u32 *fb = (u32 *)api->fb_address;
    fb_fill(fb, api->fb_pitch, api->fb_width, api->fb_height, BG);

    api->log->info("[console_fb] ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
