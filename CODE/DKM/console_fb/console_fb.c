/* DKM Console Framebuffer Driver
 * Stage 1, optional, provides "console".
 * Light-blue background + centered circular loading animation (4-color arc ring).
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
    .name="console_fb", .version="0.2.0", .vendor="Deshab",
    .driver_class=5, .stage=1, .flags=0, .priority=0,
    .depends=NULL, .depends_count=0,
    .provides=g_provides, .provides_count=1,
    .min_kernel_abi=1,
};

/* ---------------------------------------------------------------
 * Colors
 * --------------------------------------------------------------- */
static const u32 BG       = 0xFFC8E0F0; /* light blue background */
static const u32 ARC[4]   = { 0xFF4488CC, 0xFF2266AA, 0xFF66AAEE, 0xFF88CCFF };

/* ---------------------------------------------------------------
 * Drawing helpers – all integer arithmetic, no soft-float
 * --------------------------------------------------------------- */
static u64 fb_w, fb_h, fb_p;

/* double-buffer sprite for ring area (128×128 = no flicker) */
#define SPR_SIZE 128
static u32 g_sprite[SPR_SIZE * SPR_SIZE];

static void fb_fill(u32 *fb, u64 pitch, u64 w, u64 h, u32 color) {
    for (u64 y = 0; y < h; y++) {
        u32 *line = (u32 *)((u8 *)fb + y * pitch);
        for (u64 x = 0; x < w; x++) line[x] = color;
    }
}

/* alpha-blend c2 over c1, alpha 0-255 */
static u32 blend(u32 c1, u32 c2, u32 a) {
    u32 na = 256 - a;
    u32 r  = ((c1 & 0xFF) * na + (c2 & 0xFF) * a) >> 8;
    u32 g  = (((c1 >> 8) & 0xFF) * na + ((c2 >> 8) & 0xFF) * a) >> 8;
    u32 b  = (((c1 >> 16) & 0xFF) * na + ((c2 >> 16) & 0xFF) * a) >> 8;
    return 0xFF000000 | (b << 16) | (g << 8) | r;
}

/* integer sqrt – returns floor(sqrt(v)) */
static u64 isqrt(u64 v) {
    u64 x = v, y = (x + 1) >> 1;
    while (y < x) { x = y; y = (y + v / y) >> 1; }
    return x;
}

/* approximate atan2 in degrees (0=right, CCW).  dx,dy = pixel offset from center */
static i64 approx_angle(i64 dx, i64 dy) {
    if (dx == 0) return dy < 0 ? 90 : 270;
    i64 ax = dx < 0 ? -dx : dx;
    i64 ay = dy < 0 ? -dy : dy;
    i64 a;
    if (ax > ay) {
        a = ay * 45 / ax;
    } else {
        a = 90 - ax * 45 / (ay ? ay : 1);
    }
    /* quadrant correction */
    if      (dx >= 0 && dy <= 0) return a;          /* Q1 */
    else if (dx <  0 && dy <= 0) return 180 - a;     /* Q2 */
    else if (dx <  0 && dy >  0) return 180 + a;     /* Q3 */
    else                         return 360 - a;     /* Q4 */
}

/* draw one arc segment: rounded caps (6° head/tail), bright mid, comet-tail fade */
static void arc_segment(u32 *fb, i64 cx, i64 cy, i64 r, i64 start, i64 sweep, u32 color, i64 thk) {
    i64 ro = r + thk, ri = r - thk;
    i64 ri2 = ri * ri, ro2 = (ro + 1) * (ro + 1);
    i64 rnd = 6;  /* rounding zone at each end, in degrees */

    for (i64 dy = -ro - 1; dy <= ro + 1; dy++) {
        i64 y = cy + dy;
        if ((u64)y >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
        for (i64 dx = -ro - 1; dx <= ro + 1; dx++) {
            i64 x = cx + dx;
            if ((u64)x >= fb_w) continue;
            i64 d2 = dx * dx + dy * dy;
            if (d2 < ri2 - 2 || d2 > ro2 + 2) continue;

            i64 ang = approx_angle(dx, dy);
            i64 a   = ang - start;
            if (a < 0) a += 360;
            if ((u64)a > (u64)sweep) continue;

            /* rounded-cap fade: 0→255 over rnd°, then 255→0 over last rnd° */
            u32 fade;
            if ((u64)a < (u64)rnd) {
                fade = (u32)(a * 255 / rnd);
            } else if ((u64)a > (u64)(sweep - rnd)) {
                fade = (u32)((sweep - a) * 255 / rnd);
            } else {
                fade = 255;
            }

            i64 dist = (i64)isqrt((u64)d2);
            u32 effective = blend(BG, color, fade);
            if (dist >= ri && dist <= ro) {
                line[(u64)x] = effective;
            } else if (dist >= ri - 2 && dist < ri) {
                line[(u64)x] = blend(BG, effective, (u32)((dist - (ri - 2)) * 85));
            } else if (dist > ro && dist <= ro + 2) {
                line[(u64)x] = blend(effective, BG, (u32)((dist - ro) * 85));
            }
        }
    }
}

/* filled disc with anti-aliased edge */
static void fill_disc(u32 *fb, i64 cx, i64 cy, i64 r, u32 color) {
    i64 r2 = r * r, o2 = (r + 1) * (r + 1);
    for (i64 dy = -r - 2; dy <= r + 2; dy++) {
        i64 y = cy + dy;
        if ((u64)y >= fb_h) continue;
        u32 *line = (u32 *)((u8 *)fb + (u64)y * fb_p);
        for (i64 dx = -r - 2; dx <= r + 2; dx++) {
            i64 x = cx + dx;
            if ((u64)x >= fb_w) continue;
            i64 d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                line[(u64)x] = color;
            } else if (d2 <= o2) {
                u32 a = (u32)((o2 - d2) * 255 / (o2 - r2));
                line[(u64)x] = blend(BG, color, a);
            }
        }
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

    api->log->info("[console_fb] boot screen");

    fb_w = api->fb_width; fb_h = api->fb_height; fb_p = api->fb_pitch;
    u32 *fb = (u32 *)api->fb_address;
    i64 cx = (i64)fb_w / 2, cy = (i64)fb_h / 2, r = 48, thk = 4;

    /* light-blue background */
    fb_fill(fb, fb_p, fb_w, fb_h, BG);

    /* animation: 2000 frames × 2° clockwise × ~1ms, no flicker */
    i64 spr_p = SPR_SIZE * 4;
    i64 scx  = SPR_SIZE / 2;
    i64 scy  = SPR_SIZE / 2;
    i64 frame = 0, base = 0;
    i64 bx = cx - scx;  /* screen position of sprite top-left */
    i64 by = cy - scy;

    while (frame < 2000) {
        /* draw into sprite buffer — swap globals to sprite layout */
        u64 save_w = fb_w, save_h = fb_h, save_p = fb_p;
        fb_w = SPR_SIZE; fb_h = SPR_SIZE; fb_p = spr_p;

        fb_fill(g_sprite, spr_p, SPR_SIZE, SPR_SIZE, BG);

        arc_segment(g_sprite, scx, scy, r, base +  0, 82, ARC[0], thk);
        arc_segment(g_sprite, scx, scy, r, base + 90, 82, ARC[1], thk);
        arc_segment(g_sprite, scx, scy, r, base +180, 82, ARC[2], thk);
        arc_segment(g_sprite, scx, scy, r, base +270, 82, ARC[3], thk);

        fill_disc(g_sprite, scx, scy, r - 8, blend(BG, 0xFFD0F0FF, 80));

        /* restore screen globals for blit */
        fb_w = save_w; fb_h = save_h; fb_p = save_p;

        /* blit sprite to screen */
        for (i64 sr = 0; sr < SPR_SIZE; sr++) {
            i64 sy = by + sr;
            if ((u64)sy >= fb_h) continue;
            u32 *dst = (u32 *)((u8 *)fb + (u64)sy * fb_p) + bx;
            u32 *src = &g_sprite[sr * SPR_SIZE];
            for (i64 sc = 0; sc < SPR_SIZE; sc++) {
                i64 sx = bx + sc;
                if ((u64)sx >= fb_w) continue;
                dst[sc] = src[sc];
            }
        }

        for (volatile u32 d = 0; d < 72000; d++) {
            __asm__ volatile ("pause");
        }

        base -= 2;
        if (base < 0) base += 360;
        frame++;
    }

    api->log->info("[console_fb] ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
