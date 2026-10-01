/* virtio_gpu.c — UTSM virtio-gpu 2D backend (VSCode integration Phase 3).
 *
 * Implements a minimal virtio-gpu 2D device (device_id=16) so the Linux guest
 * gets /dev/dri/card0 + /dev/fb0 (via DRM fbdev emulation). The X server
 * (Xfbdev MVP / Xorg Phase 4) drives the scanout; the composited frame lands
 * in the shared graphics surface pool, which the Deshab desktop reads to blit
 * guest graphics onto the IDE host area.
 *
 * Device layout: slot 5 @ 0xF4005000 IRQ10, two queues (ctrlq + cursorq).
 *
 * Supported 2D command set (Linux uapi / Virtio 1.2 §5.7.6):
 *   GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_UNREF, SET_SCANOUT,
 *   RESOURCE_FLUSH, TRANSFER_TO_HOST_2D, RESOURCE_ATTACH_BACKING,
 *   RESOURCE_DETACH_BACKING.
 *
 * Cursor commands (UPDATE_CURSOR / MOVE_CURSOR) are acknowledged but not
 * rendered — the Deshab desktop draws its own PS/2 cursor over the blit.
 *
 * Data flow:
 *   1. Guest DRM driver creates a 2D resource, attaches guest RAM backing,
 *      binds it to scanout 0 (SET_SCANOUT), and TRANSFER_TO_HOST_2D copies a
 *      rect from guest backing into the surface pool (scanout shadow).
 *   2. RESOURCE_FLUSH marks the scanout dirty.
 *   3. The desktop polls virtio_gpu_get_scanout_info() and blits the dirty
 *      scanout to the framebuffer IDE host area, clearing dirty.
 *
 * MVP simplifications:
 *   - Single scanout (num_scanouts=1), fixed display 1024×768 XRGB8888.
 *   - Resource backing assumed contiguous (first mem_entry's GPA + total len).
 *     Guest framebuffer allocations satisfy this.
 *   - No virgl 3D, no capsets, no cross-endian byte swap (raw 32bpp copy).
 */

#include <utsm/virtio_mmio.h>
#include <utsm/linux_loader.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* ===== virtio-gpu protocol structs (must match <uapi/linux/virtio_gpu.h>) ===== */

struct virtio_gpu_config {
    u32 events_read;
    u32 events_clear;
    u32 num_scanouts;
    u32 num_capsets;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    u32 type;
    u32 flags;
    u64 fence_id;
    u32 ctx_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    u32 enabled;
    u32 flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resp_num_scanouts {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 num_scanouts;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    u64 addr;
    u32 length;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    u32 resource_id;
    u32 nr_entries;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u32 scanout_id;
    u32 resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u64 offset;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    u32 resource_id;
    u32 padding;
} __attribute__((packed));

/* Cursor VQ (Linux uapi virtio_gpu.h). pos is scanout_id/x/y, not a 2D rect. */
struct virtio_gpu_cursor_pos {
    u32 scanout_id;
    u32 x;
    u32 y;
    u32 padding;
} __attribute__((packed));

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    u32 resource_id;              /* 0 = hide cursor */
    u32 hot_x;
    u32 hot_y;
    u32 padding;
} __attribute__((packed));

#define VGPU_CURSOR_MAX 64   /* max cursor image dimension (pixels) */

/* Command types MUST match Linux include/uapi/linux/virtio_gpu.h. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT             0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH          0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO         0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET              0x0109
#define VIRTIO_GPU_CMD_GET_EDID                0x010a
#define VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID    0x010b
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB    0x010c
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB        0x010d
#define VIRTIO_GPU_CMD_UPDATE_CURSOR           0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR             0x0301

/* response types */
#define VIRTIO_GPU_RESP_OK_NODATA              0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO       0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC            0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY    0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

/* formats we accept (32bpp). Values match Linux virtio_gpu_formats. */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

/* fence flag (we just ack fences, no real fencing) */
#define VIRTIO_GPU_FLAG_FENCE (1 << 0)

/* Fixed scanout geometry advertised to the guest. 1024×768 fits in the 16MB
 * surface pool (4 MB) with headroom, and is a standard Xfbdev mode. */
#define VGPU_SCANOUT_W   1024
#define VGPU_SCANOUT_H   768
#define VGPU_BPP         4   /* XRGB8888 = 4 bytes/pixel */
#define VGPU_SCANOUT_STRIDE (VGPU_SCANOUT_W * VGPU_BPP)   /* 4096 bytes/row */
#define VGPU_SCANOUT_SIZE  (VGPU_SCANOUT_STRIDE * VGPU_SCANOUT_H)  /* 3 MB */

#define VGPU_MAX_RESOURCES 16
/* 1024×768×4 is 768 pages; keep headroom for larger dumb buffers. */
#define VGPU_MAX_BACKING_ENTRIES 2048

struct vgpu_resource {
    int in_use;
    u32 resource_id;
    u32 format;
    u32 width;
    u32 height;
    int has_backing;
    u64 backing_gpa;          /* first entry (debug / cursor fallback) */
    u64 backing_len;          /* sum of all entries */
    u32 nr_entries;
    struct virtio_gpu_mem_entry entries[VGPU_MAX_BACKING_ENTRIES];
};

/* Per-device state. Single instance (one virtio-gpu device). */
static struct {
    u64 gpa_base;
    u32 irq;

    /* config space image (16 bytes) */
    struct virtio_gpu_config cfg;

    /* resource table */
    struct vgpu_resource res[VGPU_MAX_RESOURCES];

    /* scanout 0 state */
    int scanout_enabled;
    u32 scanout_res_id;       /* 0 = none */
    u32 scanout_x, scanout_y;
    u32 scanout_w, scanout_h;

    /* surface pool (host vaddr) — composited frames land here */
    void *surface_vaddr;
    u64 surface_size;

    /* dirty: set on FLUSH of the scanout resource, cleared by get_scanout_info */
    int dirty;
    /* Phase 7: 累积 dirty rect（scanout 坐标系并集），desktop 局部 blit 用。
     * dirty_rect_valid=0 时无矩形（与 dirty=0 等价；首帧建议全量拷贝）。 */
    int dirty_rect_valid;
    u32 dirty_rx, dirty_ry, dirty_rw, dirty_rh;

    /* P7.6: hardware cursor state (cursorq UPDATE_CURSOR / MOVE_CURSOR)。
     * cursor_visible=0 直到 guest 发 UPDATE_CURSOR 且 resource_id!=0。
     * cursor_bitmap 存 ARGB 像素（row stride = VGPU_CURSOR_MAX），供 desktop
     * alpha-blend 叠加到 IDE host 区域。 */
    int cursor_visible;
    u32 cursor_x, cursor_y;       /* position on scanout */
    u32 cursor_hot_x, cursor_hot_y;
    u32 cursor_w, cursor_h;       /* actual cursor image dimensions */
    u32 cursor_bitmap[VGPU_CURSOR_MAX * VGPU_CURSOR_MAX];

    /* per-queue avail cursor */
    u16 last_avail_ctrl;
    u16 last_avail_cursor;

    /* P9: RESOURCE_FLUSH 计数（只计 scanout resource 的 flush）。
     * verify 脚本据 \[VGPU\] FLUSH_N 判断 Plasma/Xorg 是否真的推帧——
     * SET_SCANOUT 只证明 fbdev 绑定了 resource，不代表有新像素。 */
    u64 flush_count;

    struct virtio_backend backend;
} g_gpu;

/* ===== resource table helpers ===== */

static struct vgpu_resource *res_find(u32 id) {
    if (id == 0) return 0;
    for (int i = 0; i < VGPU_MAX_RESOURCES; i++) {
        if (g_gpu.res[i].in_use && g_gpu.res[i].resource_id == id)
            return &g_gpu.res[i];
    }
    return 0;
}

static struct vgpu_resource *res_alloc(void) {
    for (int i = 0; i < VGPU_MAX_RESOURCES; i++) {
        if (!g_gpu.res[i].in_use) {
            g_gpu.res[i].in_use = 1;
            return &g_gpu.res[i];
        }
    }
    return 0;
}

static void res_release(struct vgpu_resource *r) {
    if (!r) return;
    r->in_use = 0;
    r->resource_id = 0;
    r->has_backing = 0;
    r->backing_gpa = 0;
    r->backing_len = 0;
    r->nr_entries = 0;
}

/* ===== surface pool access =====
 * The scanout shadow lives at the start of the 16MB graphics surface pool
 * (allocated in linux_loader.c). virtio-gpu composites there; the desktop
 * reads the same region. Falls back to disabled scanout if the pool is absent. */
static int surface_ready(void) {
    return g_gpu.surface_vaddr != 0 && g_gpu.surface_size >= VGPU_SCANOUT_SIZE;
}

/* ===== descriptor chain walker =====
 * A virtio-gpu ctrl chain is typically: [cmd desc (read, F_NEXT)] →
 * [resp desc (write)]. We collect the first readable buffer as the command
 * and the first writable buffer as the response. Returns 0 on success and
 * fills cmd/resp host vaddrs + lens; resp may be NULL if driver omitted it. */
struct chain_bufs {
    void *cmd;    u32 cmd_len;
    void *cmd_extra; u32 cmd_extra_len; /* 2nd readable desc (attach mem_entries) */
    void *resp;   u32 resp_len;
    u16  head;    /* chain head index (for used ring) */
};

static int walk_chain(struct virtq_desc *desc, u32 qnum, u16 head,
                      struct chain_bufs *out) {
    u16 cur = head;
    int steps = 0;
    out->cmd = 0; out->cmd_len = 0;
    out->cmd_extra = 0; out->cmd_extra_len = 0;
    out->resp = 0; out->resp_len = 0;
    out->head = head;

    while (steps < 64) {   /* bound chain length */
        if (cur >= qnum) return -1;
        struct virtq_desc *d = &desc[cur];
        void *host = virtio_gpa_to_host(d->addr);
        if (!host) return -1;   /* unmapped GPA */

        if (d->flags & VIRTQ_DESC_F_WRITE) {
            if (!out->resp) { out->resp = host; out->resp_len = d->len; }
        } else {
            if (!out->cmd) {
                out->cmd = host; out->cmd_len = d->len;
            } else if (!out->cmd_extra) {
                out->cmd_extra = host; out->cmd_extra_len = d->len;
            }
        }
        if (!(d->flags & VIRTQ_DESC_F_NEXT)) break;
        cur = d->next;
        steps++;
    }
    return (out->cmd != 0) ? 0 : -1;
}

/* Write a simple NODATA response (just the ctrl_hdr). */
static void write_resp_nodata(void *resp, u32 resp_cap, u32 cmd_type, u32 resp_type) {
    if (!resp || resp_cap < (u32)sizeof(struct virtio_gpu_ctrl_hdr)) return;
    struct virtio_gpu_ctrl_hdr *h = (struct virtio_gpu_ctrl_hdr *)resp;
    h->type = resp_type;
    h->flags = 0;
    h->fence_id = 0;
    h->ctx_id = 0;
    h->padding = 0;
    (void)cmd_type;
}

/* ===== 2D command handlers =====
 * Each returns the response type to write. cmd points at the full command
 * (ctrl_hdr + command-specific body). */

static u32 handle_get_display_info(void *resp, u32 resp_cap) {
    if (!resp || resp_cap < (u32)sizeof(struct virtio_gpu_resp_display_info))
        return VIRTIO_GPU_RESP_ERR_UNSPEC;
    struct virtio_gpu_resp_display_info *r =
        (struct virtio_gpu_resp_display_info *)resp;
    /* header */
    r->hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    r->hdr.flags = 0; r->hdr.fence_id = 0; r->hdr.ctx_id = 0; r->hdr.padding = 0;
    /* one enabled scanout at (0,0,1024,768) */
    for (int i = 0; i < 16; i++) {
        r->pmodes[i].r.x = 0; r->pmodes[i].r.y = 0;
        r->pmodes[i].r.width = 0; r->pmodes[i].r.height = 0;
        r->pmodes[i].enabled = 0;
        r->pmodes[i].flags = 0;
    }
    r->pmodes[0].r.width = VGPU_SCANOUT_W;
    r->pmodes[0].r.height = VGPU_SCANOUT_H;
    r->pmodes[0].enabled = 1;
    return VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
}

static u32 handle_resource_create_2d(struct virtio_gpu_resource_create_2d *cmd) {
    if (!cmd) return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    u32 id = cmd->resource_id;
    if (id == 0) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (res_find(id)) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    /* accept 32bpp formats used by Linux DRM/dumb buffers */
    if (cmd->format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM &&
        cmd->format != VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    if (cmd->width == 0 || cmd->height == 0 ||
        cmd->width > 4096 || cmd->height > 4096)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    struct vgpu_resource *r = res_alloc();
    if (!r) return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
    r->resource_id = id;
    r->format = cmd->format;
    r->width = cmd->width;
    r->height = cmd->height;
    r->has_backing = 0;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 handle_resource_destroy(u32 resource_id) {
    struct vgpu_resource *r = res_find(resource_id);
    if (!r) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    /* if it was the scanout resource, detach */
    if (g_gpu.scanout_enabled && g_gpu.scanout_res_id == resource_id) {
        g_gpu.scanout_enabled = 0;
        g_gpu.scanout_res_id = 0;
    }
    res_release(r);
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 handle_attach_backing(struct virtio_gpu_resource_attach_backing *cmd,
                                 u32 cmd_len, void *extra, u32 extra_len) {
    if (!cmd) return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    u32 id = cmd->resource_id;
    struct vgpu_resource *r = res_find(id);
    if (!r) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    u32 n = cmd->nr_entries;
    if (n == 0 || n > VGPU_MAX_BACKING_ENTRIES)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    u32 hdr_off = (u32)sizeof(struct virtio_gpu_resource_attach_backing);
    u32 need = n * (u32)sizeof(struct virtio_gpu_mem_entry);
    struct virtio_gpu_mem_entry *entries = 0;
    if (cmd_len >= hdr_off + need) {
        entries = (struct virtio_gpu_mem_entry *)((u8 *)cmd + hdr_off);
    } else if (extra && extra_len >= need) {
        entries = (struct virtio_gpu_mem_entry *)extra;
    } else {
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    }

    u64 total = 0;
    for (u32 i = 0; i < n; i++) {
        r->entries[i] = entries[i];
        total += entries[i].length;
    }
    r->nr_entries = n;
    r->has_backing = 1;
    r->backing_gpa = entries[0].addr;
    r->backing_len = total;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 handle_detach_backing(u32 resource_id) {
    struct vgpu_resource *r = res_find(resource_id);
    if (!r) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    r->has_backing = 0;
    r->backing_gpa = 0;
    r->backing_len = 0;
    r->nr_entries = 0;
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 handle_set_scanout(struct virtio_gpu_set_scanout *cmd) {
    if (!cmd) return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    if (cmd->scanout_id != 0) return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;

    if (cmd->resource_id == 0) {
        /* disabling scanout */
        g_gpu.scanout_enabled = 0;
        g_gpu.scanout_res_id = 0;
        return VIRTIO_GPU_RESP_OK_NODATA;
    }
    struct vgpu_resource *r = res_find(cmd->resource_id);
    if (!r) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    if (cmd->r.width == 0 || cmd->r.height == 0)
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

    g_gpu.scanout_enabled = 1;
    g_gpu.scanout_res_id = cmd->resource_id;
    g_gpu.scanout_x = cmd->r.x;
    g_gpu.scanout_y = cmd->r.y;
    g_gpu.scanout_w = cmd->r.width;
    g_gpu.scanout_h = cmd->r.height;
    g_gpu.dirty = 1;
    log_info("[VGPU] SET_SCANOUT");
    log_hex64("[VGPU] scanout w=", g_gpu.scanout_w);
    log_hex64("[VGPU] scanout h=", g_gpu.scanout_h);
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/* Copy `len` bytes at resource-relative `off` from scatter-gather backing. */
static int backing_copy(struct vgpu_resource *r, u64 off, u32 len, u8 *dst) {
    u64 pos = 0;
    u32 i;
    if (!r || !dst || !r->has_backing || len == 0) return -1;
    for (i = 0; i < r->nr_entries && len; i++) {
        u64 elen = r->entries[i].length;
        if (off >= pos + elen) {
            pos += elen;
            continue;
        }
        {
            u64 skip = (off > pos) ? (off - pos) : 0;
            u64 take = elen - skip;
            void *host;
            u8 *s;
            u32 k;
            if (take > len) take = len;
            host = virtio_gpa_to_host(r->entries[i].addr + skip);
            if (!host) return -1;
            s = (u8 *)host;
            for (k = 0; k < (u32)take; k++) dst[k] = s[k];
            dst += take;
            len -= (u32)take;
            off += take;
            pos += elen;
        }
    }
    return (len == 0) ? 0 : -1;
}

/* Copy a rect of a resource from guest backing memory into the surface pool
 * scanout region. Called for both TRANSFER_TO_HOST_2D and (implicitly via flush)
 * — we do the actual copy on TRANSFER, flush just marks dirty. */
static void composite_rect(struct vgpu_resource *r, struct virtio_gpu_rect *rect,
                           u64 offset) {
    if (!surface_ready() || !r || !r->has_backing) return;
    /* only composite the resource currently bound to the scanout */
    if (!g_gpu.scanout_enabled || g_gpu.scanout_res_id != r->resource_id) return;

    u32 src_stride = r->width * VGPU_BPP;
    u32 dst_stride = VGPU_SCANOUT_STRIDE;   /* scanout is fixed 1024×768 */

    /* clamp rect to resource bounds */
    u32 rx = rect->x, ry = rect->y, rw = rect->width, rh = rect->height;
    if (rx >= r->width || ry >= r->height) return;
    if (rx + rw > r->width)  rw = r->width - rx;
    if (ry + rh > r->height) rh = r->height - ry;
    /* clamp to scanout bounds */
    u32 dx = g_gpu.scanout_x + rx;
    u32 dy = g_gpu.scanout_y + ry;
    if (dx >= VGPU_SCANOUT_W || dy >= VGPU_SCANOUT_H) return;
    if (dx + rw > VGPU_SCANOUT_W) rw = VGPU_SCANOUT_W - dx;
    if (dy + rh > VGPU_SCANOUT_H) rh = VGPU_SCANOUT_H - dy;
    if (rw == 0 || rh == 0) return;

    u8 *dst = (u8 *)g_gpu.surface_vaddr;
    u32 copy_bytes = rw * VGPU_BPP;
    static u8 rowbuf[4096];
    if (copy_bytes > (u32)sizeof(rowbuf)) copy_bytes = (u32)sizeof(rowbuf);

    for (u32 row = 0; row < rh; row++) {
        u64 src_off = offset + (u64)(ry + row) * src_stride + (u64)rx * VGPU_BPP;
        u8 *d = dst + (u64)(dy + row) * dst_stride + (u64)dx * VGPU_BPP;
        if (backing_copy(r, src_off, copy_bytes, rowbuf) != 0) break;
        {
            u32 n = copy_bytes / 4;
            u32 *sp = (u32 *)rowbuf;
            u32 *dp = (u32 *)d;
            for (u32 i = 0; i < n; i++) dp[i] = sp[i];
        }
    }
}

static u32 handle_transfer_to_host_2d(struct virtio_gpu_transfer_to_host_2d *cmd) {
    if (!cmd) return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    struct vgpu_resource *r = res_find(cmd->resource_id);
    if (!r) return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
    composite_rect(r, &cmd->r, cmd->offset);
    return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 handle_resource_flush(struct virtio_gpu_resource_flush *cmd) {
    if (!cmd) return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    /* mark scanout dirty so the desktop re-blits. We only flushed the scanout
     * resource (composite_rect already copied it on TRANSFER). */
    if (g_gpu.scanout_enabled && g_gpu.scanout_res_id == cmd->resource_id) {
        g_gpu.dirty = 1;
        g_gpu.flush_count++;
        /* P9: 每 64 次 flush 打一行计数。串口噪音远小于逐次打印；
         * verify 用 FLUSH_N 出现判断 guest 真的在推帧。 */
        if ((g_gpu.flush_count & 0x3F) == 1) {
            log_info("[VGPU] FLUSH_N");
            log_hex64("[VGPU] flush count=", g_gpu.flush_count);
        }
        /* Phase 7: 把 flush rect（resource 坐标系）并入累积 dirty rect
         * （scanout 坐标系并集），desktop 据此只 blit 变化区域。
         * 钳制规则与 composite_rect 一致。 */
        u32 rx = cmd->r.x, ry = cmd->r.y, rw = cmd->r.width, rh = cmd->r.height;
        u32 dx = g_gpu.scanout_x + rx;
        u32 dy = g_gpu.scanout_y + ry;
        if (dx < VGPU_SCANOUT_W && dy < VGPU_SCANOUT_H && rw > 0 && rh > 0) {
            if (dx + rw > VGPU_SCANOUT_W) rw = VGPU_SCANOUT_W - dx;
            if (dy + rh > VGPU_SCANOUT_H) rh = VGPU_SCANOUT_H - dy;
            if (!g_gpu.dirty_rect_valid) {
                g_gpu.dirty_rx = dx; g_gpu.dirty_ry = dy;
                g_gpu.dirty_rw = rw; g_gpu.dirty_rh = rh;
                g_gpu.dirty_rect_valid = 1;
            } else {
                /* 并集：expand 到同时包含旧矩形与新矩形 */
                u32 x0 = (dx < g_gpu.dirty_rx) ? dx : g_gpu.dirty_rx;
                u32 y0 = (dy < g_gpu.dirty_ry) ? dy : g_gpu.dirty_ry;
                u32 x1 = ((dx + rw) > (g_gpu.dirty_rx + g_gpu.dirty_rw))
                       ? (dx + rw) : (g_gpu.dirty_rx + g_gpu.dirty_rw);
                u32 y1 = ((dy + rh) > (g_gpu.dirty_ry + g_gpu.dirty_rh))
                       ? (dy + rh) : (g_gpu.dirty_ry + g_gpu.dirty_rh);
                g_gpu.dirty_rx = x0; g_gpu.dirty_ry = y0;
                g_gpu.dirty_rw = x1 - x0; g_gpu.dirty_rh = y1 - y0;
            }
        }
    }
    return VIRTIO_GPU_RESP_OK_NODATA;
}

/* ===== ctrlq processing ===== */

static void gpu_ctrlq_process(void) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs_by_gpa(g_gpu.gpa_base, 0,
            &desc, &avail, &used, &qnum) != 0)
        return;

    u16 cur = g_gpu.last_avail_ctrl;
    u16 used_idx = used->idx;
    int budget = 32;   /* bound work per notify */
    int filled = 0;

    while (budget-- > 0 && cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        if (head >= qnum) { cur++; continue; }

        struct chain_bufs cb;
        if (walk_chain(desc, qnum, head, &cb) != 0) {
            /* bad chain: return it empty so the driver recycles */
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = 0;
            used_idx++; cur++; filled++;
            continue;
        }

        struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cb.cmd;
        u32 resp_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
        u32 resp_len = (u32)sizeof(struct virtio_gpu_ctrl_hdr);   /* default NODATA */

        if (cb.cmd_len < (u32)sizeof(struct virtio_gpu_ctrl_hdr)) {
            resp_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
        } else {
            u32 t = hdr->type;
            switch (t) {
            case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
                resp_type = handle_get_display_info(cb.resp, cb.resp_len);
                if (resp_type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
                    resp_len = (u32)sizeof(struct virtio_gpu_resp_display_info);
                break;
            case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_resource_create_2d))
                    resp_type = handle_resource_create_2d(
                        (struct virtio_gpu_resource_create_2d *)cb.cmd);
                else
                    resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
                u32 rid = 0;
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_ctrl_hdr) + 4)
                    rid = *(u32 *)((u8 *)cb.cmd + sizeof(struct virtio_gpu_ctrl_hdr));
                resp_type = handle_resource_destroy(rid);
                break;
            }
            case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_resource_attach_backing))
                    resp_type = handle_attach_backing(
                        (struct virtio_gpu_resource_attach_backing *)cb.cmd,
                        cb.cmd_len, cb.cmd_extra, cb.cmd_extra_len);
                else
                    resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
                u32 rid = 0;
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_ctrl_hdr) + 4)
                    rid = *(u32 *)((u8 *)cb.cmd + sizeof(struct virtio_gpu_ctrl_hdr));
                resp_type = handle_detach_backing(rid);
                break;
            }
            case VIRTIO_GPU_CMD_SET_SCANOUT:
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_set_scanout))
                    resp_type = handle_set_scanout(
                        (struct virtio_gpu_set_scanout *)cb.cmd);
                else
                    resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_transfer_to_host_2d))
                    resp_type = handle_transfer_to_host_2d(
                        (struct virtio_gpu_transfer_to_host_2d *)cb.cmd);
                else
                    resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
                if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_resource_flush))
                    resp_type = handle_resource_flush(
                        (struct virtio_gpu_resource_flush *)cb.cmd);
                else
                    resp_type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
            case VIRTIO_GPU_CMD_GET_CAPSET:
            case VIRTIO_GPU_CMD_GET_EDID:
            case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
            case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
            case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
                /* No virgl/blob/EDID features advertised; reject cleanly. */
                resp_type = VIRTIO_GPU_RESP_ERR_UNSPEC;
                break;
            default: {
                static u32 last_unknown;
                if (t != last_unknown) {
                    last_unknown = t;
                    log_warn("[VGPU] unknown ctrl cmd");
                    log_hex64("  type=", t);
                }
                resp_type = VIRTIO_GPU_RESP_OK_NODATA;
                break;
            }
            }
        }

        /* write the response (unless GET_* already wrote a richer response) */
        if (resp_type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO &&
            resp_type != VIRTIO_GPU_RESP_OK_NODATA) {
            write_resp_nodata(cb.resp, cb.resp_len, hdr->type, resp_type);
            resp_len = (u32)sizeof(struct virtio_gpu_ctrl_hdr);
        } else if (resp_type == VIRTIO_GPU_RESP_OK_NODATA &&
                   hdr->type != VIRTIO_GPU_CMD_GET_DISPLAY_INFO) {
            write_resp_nodata(cb.resp, cb.resp_len, hdr->type, resp_type);
            resp_len = (u32)sizeof(struct virtio_gpu_ctrl_hdr);
        }

        /* fence ack: if the command requested a fence, echo fence_id in resp */
        if ((hdr->flags & VIRTIO_GPU_FLAG_FENCE) && cb.resp &&
            cb.resp_len >= (u32)sizeof(struct virtio_gpu_ctrl_hdr)) {
            struct virtio_gpu_ctrl_hdr *rh = (struct virtio_gpu_ctrl_hdr *)cb.resp;
            rh->flags |= VIRTIO_GPU_FLAG_FENCE;
            rh->fence_id = hdr->fence_id;
        }

        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = resp_len;
        used_idx++; cur++; filled++;
    }

    if (filled > 0) {
        virtio_queue_bump_used(used, used_idx);
        g_gpu.last_avail_ctrl = cur;
        virtio_mmio_raise_irq_by_gpa(g_gpu.gpa_base);
    }
}

/* P7.6: cursorq - process UPDATE_CURSOR / MOVE_CURSOR.
 * UPDATE_CURSOR: if resource_id != 0, copy cursor image from resource backing
 * (up to 64x64 ARGB). If resource_id == 0, hide cursor.
 * MOVE_CURSOR: update cursor position on scanout. */
static void gpu_cursorq_process(void) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs_by_gpa(g_gpu.gpa_base, 1,
            &desc, &avail, &used, &qnum) != 0)
        return;

    u16 cur = g_gpu.last_avail_cursor;
    u16 used_idx = used->idx;
    int budget = 16;
    int filled = 0;

    while (budget-- > 0 && cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        if (head >= qnum) { cur++; continue; }

        struct chain_bufs cb;
        if (walk_chain(desc, qnum, head, &cb) == 0 && cb.cmd && cb.resp) {
            struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)cb.cmd;

            if (cb.cmd_len >= (u32)sizeof(struct virtio_gpu_update_cursor)) {
                struct virtio_gpu_update_cursor *cmd =
                    (struct virtio_gpu_update_cursor *)cb.cmd;

                if (hdr->type == VIRTIO_GPU_CMD_UPDATE_CURSOR) {
                    u32 rid = cmd->resource_id;
                    g_gpu.cursor_x = cmd->pos.x;
                    g_gpu.cursor_y = cmd->pos.y;
                    g_gpu.cursor_hot_x = cmd->hot_x;
                    g_gpu.cursor_hot_y = cmd->hot_y;

                    if (rid != 0) {
                        struct vgpu_resource *r = res_find(rid);
                        if (r && r->has_backing && r->width > 0 && r->height > 0) {
                            void *host = virtio_gpa_to_host(r->backing_gpa);
                            if (host) {
                                int cw = (int)r->width;
                                int ch = (int)r->height;
                                if (cw > VGPU_CURSOR_MAX) cw = VGPU_CURSOR_MAX;
                                if (ch > VGPU_CURSOR_MAX) ch = VGPU_CURSOR_MAX;
                                u32 *src = (u32 *)host;
                                for (int row = 0; row < ch; row++) {
                                    for (int col = 0; col < cw; col++) {
                                        g_gpu.cursor_bitmap[row * VGPU_CURSOR_MAX + col] =
                                            src[row * (int)r->width + col];
                                    }
                                }
                                g_gpu.cursor_w = (u32)cw;
                                g_gpu.cursor_h = (u32)ch;
                                g_gpu.cursor_visible = 1;
                            } else {
                                /* backing GPA unmapped: still show at pos, no image */
                                g_gpu.cursor_visible = 0;
                            }
                        } else {
                            g_gpu.cursor_visible = 0;
                        }
                    } else {
                        /* resource_id == 0: hide cursor */
                        g_gpu.cursor_visible = 0;
                    }
                } else if (hdr->type == VIRTIO_GPU_CMD_MOVE_CURSOR) {
                    g_gpu.cursor_x = cmd->pos.x;
                    g_gpu.cursor_y = cmd->pos.y;
                    /* MOVE_CURSOR does not change visibility or image */
                }
            }

            write_resp_nodata(cb.resp, cb.resp_len, hdr->type, VIRTIO_GPU_RESP_OK_NODATA);
        }
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = (u32)sizeof(struct virtio_gpu_ctrl_hdr);
        used_idx++; cur++; filled++;
    }

    if (filled > 0) {
        virtio_queue_bump_used(used, used_idx);
        g_gpu.last_avail_cursor = cur;
        virtio_mmio_raise_irq_by_gpa(g_gpu.gpa_base);
    }
}

/* ===== virtio_backend interface ===== */

static u32 gpu_read_config(u32 offset, int width) {
    /* config space is 16 bytes, little-endian u32 fields. Guest reads u32. */
    if (width != 4) return 0;
    if (offset >= (u32)sizeof(struct virtio_gpu_config)) return 0;
    const u8 *p = (const u8 *)&g_gpu.cfg;
    u32 v;
    v = (u32)p[offset] | ((u32)p[offset+1] << 8) |
        ((u32)p[offset+2] << 16) | ((u32)p[offset+3] << 24);
    return v;
}

static void gpu_write_config(u32 offset, u32 value) {
    /* events_clear: writing a bit clears it in events_read. We have no events,
     * so this is effectively a no-op, but honor the w1c semantics. */
    if (offset == 4) {   /* events_clear */
        g_gpu.cfg.events_read &= ~value;
    }
    (void)offset;
}

static void gpu_queue_notify(u32 queue_idx) {
    if (queue_idx == 0) {
        gpu_ctrlq_process();
    } else if (queue_idx == 1) {
        gpu_cursorq_process();
    }
}

static void gpu_reset(void) {
    g_gpu.cfg.events_read = 0;
    g_gpu.cfg.events_clear = 0;
    g_gpu.cfg.num_scanouts = 1;
    g_gpu.cfg.num_capsets = 0;
    g_gpu.scanout_enabled = 0;
    g_gpu.scanout_res_id = 0;
    g_gpu.scanout_w = 0; g_gpu.scanout_h = 0;
    g_gpu.dirty = 0;
    g_gpu.dirty_rect_valid = 0;
    g_gpu.dirty_rx = 0; g_gpu.dirty_ry = 0;
    g_gpu.dirty_rw = 0; g_gpu.dirty_rh = 0;
    g_gpu.cursor_visible = 0;
    g_gpu.cursor_x = 0; g_gpu.cursor_y = 0;
    g_gpu.cursor_hot_x = 0; g_gpu.cursor_hot_y = 0;
    g_gpu.cursor_w = 0; g_gpu.cursor_h = 0;
    g_gpu.last_avail_ctrl = 0;
    g_gpu.last_avail_cursor = 0;
    g_gpu.flush_count = 0;
    for (int i = 0; i < VGPU_MAX_RESOURCES; i++) res_release(&g_gpu.res[i]);
}

static struct virtio_backend g_gpu_backend = {
    .device_id      = VIRTIO_ID_GPU,
    .gpa_base       = VIRTIO_MMIO_GPU_GPA,
    .irq            = 10,                    /* cmdline :10, vector 0x3A */
    .num_queues     = 2,                     /* ctrlq + cursorq */
    .queue_size     = 64,
    .device_features = (1ULL << VIRTIO_F_VERSION_1),
    .config_len     = (u32)sizeof(struct virtio_gpu_config),
    .read_config    = gpu_read_config,
    .write_config   = gpu_write_config,
    .queue_notify   = gpu_queue_notify,
    .reset          = gpu_reset,
};

/* ===== public API ===== */

int virtio_gpu_get_scanout_info(struct virtio_gpu_scanout_info *out) {
    if (!out) return -1;
    if (!surface_ready()) {
        out->host_vaddr = 0;
        out->width = 0; out->height = 0; out->stride = 0;
        out->dirty = 0; out->enabled = 0;
        out->dirty_x = 0; out->dirty_y = 0;
        out->dirty_w = 0; out->dirty_h = 0;
        out->cursor_visible = 0; out->cursor_x = 0; out->cursor_y = 0;
        out->cursor_hot_x = 0; out->cursor_hot_y = 0;
        out->cursor_w = 0; out->cursor_h = 0; out->cursor_bitmap = 0;
        return -1;
    }
    out->host_vaddr = g_gpu.surface_vaddr;
    out->width = VGPU_SCANOUT_W;
    out->height = VGPU_SCANOUT_H;
    out->stride = VGPU_SCANOUT_STRIDE;
    out->enabled = (u32)g_gpu.scanout_enabled;
    out->dirty = (u32)g_gpu.dirty;
    /* Phase 7: 输出累积 dirty rect（查询一并清零）。
     * dirty=1 但 rect 无效（理论不出现）时给全帧矩形兜底。 */
    if (g_gpu.dirty && g_gpu.dirty_rect_valid) {
        out->dirty_x = g_gpu.dirty_rx;
        out->dirty_y = g_gpu.dirty_ry;
        out->dirty_w = g_gpu.dirty_rw;
        out->dirty_h = g_gpu.dirty_rh;
    } else if (g_gpu.dirty) {
        out->dirty_x = 0; out->dirty_y = 0;
        out->dirty_w = VGPU_SCANOUT_W; out->dirty_h = VGPU_SCANOUT_H;
    } else {
        out->dirty_x = 0; out->dirty_y = 0;
        out->dirty_w = 0; out->dirty_h = 0;
    }
    g_gpu.dirty = 0;   /* query consumes the dirty flag */
    g_gpu.dirty_rect_valid = 0;
    g_gpu.dirty_rw = 0; g_gpu.dirty_rh = 0;

    /* P7.6: cursor state (not consumed by query - desktop reads every frame) */
    out->cursor_visible = (u32)g_gpu.cursor_visible;
    out->cursor_x = g_gpu.cursor_x;
    out->cursor_y = g_gpu.cursor_y;
    out->cursor_hot_x = g_gpu.cursor_hot_x;
    out->cursor_hot_y = g_gpu.cursor_hot_y;
    out->cursor_w = g_gpu.cursor_w;
    out->cursor_h = g_gpu.cursor_h;
    out->cursor_bitmap = g_gpu.cursor_visible ? g_gpu.cursor_bitmap : 0;
    return 0;
}

void virtio_gpu_backend_init(void) {
    g_gpu.gpa_base = VIRTIO_MMIO_GPU_GPA;
    g_gpu.irq = 10;

    gpu_reset();   /* sets cfg + clears resource table */

    /* Bind the surface pool as the scanout shadow buffer. */
    g_gpu.surface_vaddr = linux_get_surface_vaddr();
    u64 hpa = 0, gpa = 0, size = 0;
    if (linux_get_surface_info(&hpa, &gpa, &size) == 0) {
        g_gpu.surface_size = size;
    } else {
        g_gpu.surface_size = 0;
    }

    if (!surface_ready()) {
        log_warn("[VGPU] surface pool unavailable — scanout disabled");
    } else {
        log_info("[VGPU] virtio-gpu 2D backend ready (1024x768 XRGB8888)");
    }

    virtio_mmio_register(&g_gpu_backend);
}
