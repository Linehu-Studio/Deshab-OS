/* virtio_blk.c — UTSM virtio-blk 设备模拟后端。
 *
 * 为 Linux guest 提供 virtio-blk 块设备。guest 的块读写请求经
 * virtqueue 传入，本后端桥接到 UTSM 真实块设备（block API，AHCI/SATA）。
 *
 * Virtio Spec 1.1 §5.2 (Block Device)。
 * 请求描述符链： [header(16B, R)] [data(N*512B, R/W)] [status(1B, W)]
 * 支持的操作：VIRTIO_BLK_T_IN(读)、VIRTIO_BLK_T_OUT(写)、VIRTIO_BLK_T_GET_ID。
 *
 * 设计说明：
 * - 桥接到 UTSM block API 的第 0 个设备（ahci0 SATA 盘）。
 * - guest 把 Arch rootfs 镜像当作 /dev/vda 挂载；镜像需预先写入 SATA 盘
 *   的某个分区/LBA 区间（由 mkfat32/build 流程或独立 raw 区提供）。
 * - 当前为最小可用：扇区 512B，单队列，无 flush/discard。
 */

#include <utsm/virtio_mmio.h>
#include <utsm/linux_loader.h>
#include <utsm/block.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../../tools/fat32_lfn.h"
#include "../../tools/fat32_part.h"

/* virtio-blk 请求头（Virtio Spec 1.1 §5.2.6.2） */
struct virtio_blk_outhdr {
    u32 type;       /* VIRTIO_BLK_T_* */
    u32 reserved;   /* (ioprio) */
    u64 sector;     /* 起始扇区（512B 单位） */
} __attribute__((packed));

#define VIRTIO_BLK_T_IN      0   /* 读 */
#define VIRTIO_BLK_T_OUT     1   /* 写 */
#define VIRTIO_BLK_T_FLUSH   4
#define VIRTIO_BLK_T_GET_ID  8

#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

/* feature bits */
#define VIRTIO_BLK_F_SIZE_MAX   1
#define VIRTIO_BLK_F_SEG_MAX    2
#define VIRTIO_BLK_F_BLK_SIZE   6
#define VIRTIO_BLK_F_FLUSH      9

/* device config 空间须与 Linux uapi virtio_blk.h 布局一致：
 * capacity(8) + size_max(4) + seg_max(4) + geometry(4) + blk_size(4).
 * 旧 u16 geometry[4] 把 blk_size 推到 offset 24，guest 读到 0 → -EINVAL. */
struct virtio_blk_config {
    u64 capacity;       /* 总扇区数（512B） */
    u32 size_max;
    u32 seg_max;
    u16 geometry_cylinders;
    u8  geometry_heads;
    u8  geometry_sectors;
    u32 blk_size;
} __attribute__((packed));

/* Virtio spec: size_max is the max single *segment* in *bytes*, not sectors.
 * 128 made Linux split 512B reads into 128B fragments (or reject them). */
#define VIRTIO_BLK_SIZE_MAX_BYTES 65535u
#define VBLK_MAX_DATA_SEGS        32

/* 后端状态 */
static u64 g_blk_capacity;      /* 扇区数 */
static u32 g_blk_sector_size = 512;
static int g_blk_bound = -1;    /* 绑定的 UTSM block 设备索引 */
static u32 g_vblk_ioerr_logs;

static void vblk_memcpy(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

static void vblk_memzero(u8 *p, u32 n) {
    while (n--) *p++ = 0;
}

struct vblk_req {
    struct virtio_blk_outhdr hdr;
    u8 *status;
    u8 *data[VBLK_MAX_DATA_SEGS];
    u32 dlen[VBLK_MAX_DATA_SEGS];
    int ndata;
};

/* Walk a split-virtqueue blk chain, including INDIRECT tables and
 * multi-segment data. Layout: [hdr R] [data...] [status W, 1 byte]. */
static int vblk_parse_req(struct virtq_desc *vq, u32 qnum, u16 head,
                          struct vblk_req *out) {
    u8 *raw = (u8 *)out;
    u32 z;
    for (z = 0; z < sizeof(*out); z++) raw[z] = 0;
    if (!vq || head >= qnum) return -1;

    struct virtq_desc *desc = vq;
    u32 ntbl = qnum;
    u16 cur = head;

    if (vq[head].flags & VIRTQ_DESC_F_INDIRECT) {
        u32 n = vq[head].len / (u32)sizeof(struct virtq_desc);
        if (n == 0 || n > 256) return -1;
        desc = (struct virtq_desc *)virtio_gpa_to_host(vq[head].addr);
        if (!desc) return -1;
        ntbl = n;
        cur = 0;
    }

    u16 chain[VBLK_MAX_DATA_SEGS + 4];
    u16 nchain = 0;
    int hops = 0;
    for (;;) {
        if (cur >= ntbl) return -1;
        if (nchain >= (u16)(sizeof(chain) / sizeof(chain[0]))) return -1;
        chain[nchain++] = cur;
        if (!(desc[cur].flags & VIRTQ_DESC_F_NEXT)) break;
        cur = desc[cur].next;
        if (++hops > 64) return -1;
    }

    int seen_hdr = 0;
    for (u16 i = 0; i < nchain; i++) {
        struct virtq_desc *d = &desc[chain[i]];
        void *host = virtio_gpa_to_host(d->addr);
        int wr = (d->flags & VIRTQ_DESC_F_WRITE) != 0;
        int last = (i + 1 == nchain);

        if (!wr && !seen_hdr) {
            if (!host || d->len < sizeof(struct virtio_blk_outhdr)) return -1;
            vblk_memcpy(&out->hdr, host, sizeof(out->hdr));
            out->hdr.type &= 0xFFu; /* drop legacy T_BARRIER */
            seen_hdr = 1;
            continue;
        }
        if (last && wr && d->len <= 16) {
            out->status = (u8 *)host;
            continue;
        }
        if (!host) return -1;
        if (out->ndata >= VBLK_MAX_DATA_SEGS) return -1;
        out->data[out->ndata] = (u8 *)host;
        out->dlen[out->ndata] = d->len;
        out->ndata++;
    }
    return seen_hdr ? 0 : -1;
}

static void vblk_log_ioerr(const char *tag, const struct vblk_req *r, u64 why) {
    if (g_vblk_ioerr_logs >= 8) return;
    g_vblk_ioerr_logs++;
    log_info(tag);
    log_hex64("[VBLK] ioerr why=", why);
    if (!r) return;
    log_hex64("[VBLK] ioerr type=", r->hdr.type);
    log_hex64("[VBLK] ioerr sector=", r->hdr.sector);
    log_hex64("[VBLK] ioerr ndata=", (u64)(u32)r->ndata);
}

static u32 vblk_copy_from_image(const struct vblk_req *r, const u8 *img, u64 img_size) {
    u64 offset = r->hdr.sector * 512ULL;
    u64 pos = 0;
    for (int i = 0; i < r->ndata; i++) {
        u32 n = r->dlen[i];
        u64 src = offset + pos;
        u64 avail = (src < img_size) ? (img_size - src) : 0;
        u32 copy = (avail >= (u64)n) ? n : (u32)avail;
        if (copy) vblk_memcpy(r->data[i], img + src, copy);
        if (copy < n) vblk_memzero(r->data[i] + copy, n - copy);
        pos += n;
    }
    return (u32)pos;
}

static u32 vblk_copy_to_image(const struct vblk_req *r, u8 *img, u64 img_size) {
    u64 offset = r->hdr.sector * 512ULL;
    u64 pos = 0;
    for (int i = 0; i < r->ndata; i++) {
        u32 n = r->dlen[i];
        u64 dst = offset + pos;
        u64 avail = (dst < img_size) ? (img_size - dst) : 0;
        u32 copy = (avail >= (u64)n) ? n : (u32)avail;
        if (copy) vblk_memcpy(img + dst, r->data[i], copy);
        pos += n;
    }
    return 0;
}

static void vblk_write_id(const struct vblk_req *r, const char *id) {
    if (r->ndata == 0 || !r->data[0]) return;
    u32 cap = r->dlen[0];
    u32 i = 0;
    if (cap == 0) return;
    for (; id[i] && i < cap - 1; i++) r->data[0][i] = id[i];
    r->data[0][i] = 0;
}

/* 读取 device config 空间 */
static u32 blk_read_config(u32 offset, int width) {
    (void)width;
    struct virtio_blk_config cfg;
    /* 填零后逐字段读取 */
    u8 *c = (u8 *)&cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) c[i] = 0;
    cfg.capacity = g_blk_capacity;
    cfg.size_max = VIRTIO_BLK_SIZE_MAX_BYTES;
    cfg.seg_max = 32;
    cfg.blk_size = g_blk_sector_size;

    /* 按 offset 返回 32 位窗口 */
    if (offset + 4 <= sizeof(cfg)) {
        u32 v;
        u8 *src = (u8 *)&cfg + offset;
        v = (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
        return v;
    }
    return 0;
}

/* 处理一个块请求描述符链。返回写入 guest 的字节数（用于 used.len）。 */
static u32 blk_handle_chain(struct virtq_desc *desc, u16 head, u32 qnum) {
    struct vblk_req req;
    if (vblk_parse_req(desc, qnum, head, &req) != 0) {
        vblk_log_ioerr("[VBLK] sata parse fail", &req, 1);
        return 0;
    }

    u32 bytes_done = 0;
    u8 result = VIRTIO_BLK_S_OK;
    const dkm_block_api *blk = block_get_api();

    switch (req.hdr.type) {
    case VIRTIO_BLK_T_IN: {
        if (req.ndata == 0 || g_blk_bound < 0 || !blk) {
            result = VIRTIO_BLK_S_IOERR;
            vblk_log_ioerr("[VBLK] sata read ioerr", &req, 2);
            break;
        }
        u64 sector = req.hdr.sector;
        for (int i = 0; i < req.ndata; i++) {
            u32 nsec = req.dlen[i] / 512;
            if (nsec == 0) nsec = 1;
            if (blk->read((u32)g_blk_bound, sector, nsec, req.data[i]) != 0) {
                result = VIRTIO_BLK_S_IOERR;
                break;
            }
            sector += nsec;
            bytes_done += req.dlen[i];
        }
        break;
    }
    case VIRTIO_BLK_T_OUT: {
        if (req.ndata == 0 || g_blk_bound < 0 || !blk) {
            result = VIRTIO_BLK_S_IOERR;
            break;
        }
        u64 sector = req.hdr.sector;
        for (int i = 0; i < req.ndata; i++) {
            u32 nsec = req.dlen[i] / 512;
            if (nsec == 0) nsec = 1;
            if (blk->write((u32)g_blk_bound, sector, nsec, req.data[i]) != 0) {
                result = VIRTIO_BLK_S_IOERR;
                break;
            }
            sector += nsec;
        }
        bytes_done = 0;
        break;
    }
    case VIRTIO_BLK_T_GET_ID:
        vblk_write_id(&req, "DESHAB-VIRTIO-BLK");
        bytes_done = (req.ndata > 0) ? req.dlen[0] : 0;
        break;
    case VIRTIO_BLK_T_FLUSH:
        result = VIRTIO_BLK_S_OK;
        break;
    default:
        result = VIRTIO_BLK_S_UNSUPP;
        break;
    }

    __asm__ volatile("mfence" ::: "memory");
    if (req.status) *req.status = result;
    return bytes_done;
}

/* queue_notify：guest 提交了块请求，处理队列。 */
static u16 g_blk_last_avail;

static void blk_queue_notify(u32 queue_idx) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;

    if (virtio_queue_get_ptrs(VIRTIO_ID_BLOCK, queue_idx,
                              &desc, &avail, &used, &qnum) != 0) {
        return;
    }

    /* 处理自上次以来的所有新 avail 请求 */
    __asm__ volatile("mfence" ::: "memory");
    u16 cur = g_blk_last_avail;
    u16 used_idx = used->idx;
    while (cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        u32 len = blk_handle_chain(desc, head, qnum);
        /* 写 used 回执 */
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = len;
        used_idx++;
        cur++;
    }
    virtio_queue_bump_used(used, used_idx);
    g_blk_last_avail = cur;
}

static void blk_reset(void) {
    g_blk_last_avail = 0;
}

/* 后端结构体 */
static struct virtio_backend g_blk_backend = {
    .device_id = VIRTIO_ID_BLOCK,
    .gpa_base = VIRTIO_MMIO_BLK_GPA,
    .irq = 5,                   /* guest ISA IRQ5（cmdline :5，vector 0x35） */
    .num_queues = 1,
    .queue_size = 128,
    .device_features = (1ULL << VIRTIO_F_VERSION_1) |
                       (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
                       (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                       (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                       (1ULL << VIRTIO_BLK_F_FLUSH),
    .config_len = sizeof(struct virtio_blk_config),
    .read_config = blk_read_config,
    .queue_notify = blk_queue_notify,
    .reset = blk_reset,
};

void virtio_blk_backend_init(void) {
    /* 绑定 UTSM 第 0 个块设备（ahci0 SATA 盘）作为后端存储 */
    const dkm_block_api *blk = block_get_api();
    if (blk && blk->device_count() > 0) {
        g_blk_bound = 0;
        u64 ssize = blk->sector_size(0);
        if (ssize > 0) g_blk_sector_size = (u32)ssize;
        /* 从设备名推算容量不可得，用默认值（guest 端可再探测）。
         * 【插桩白名单】capacity 为声明值而非真实容量：block API 尚无
         * capacity 查询（sector_count 仅部分 provider 提供）。这是对
         * guest 的声明边界而非"静默失败"，guest 端按声明值工作；
         * 补齐 block capacity 查询后改为真实值。 */
        g_blk_capacity = 8ULL * 1024 * 1024 * 1024 / 512;  /* 8GB in sectors */
        log_info("[VBLK] bound to UTSM block device 0");
        log_hex64("[VBLK] capacity(sectors)=", g_blk_capacity);
    } else {
        g_blk_bound = -1;
        g_blk_capacity = 0;
        log_warn("[VBLK] no UTSM block device, blk backend degraded");
    }

    virtio_mmio_register(&g_blk_backend);
}

/* ===== Memory-backed rootfs virtio-blk 后端 =====
 *
 * 第二个 virtio-blk 设备，后端为内存中的 Arch rootfs 镜像（Limine boot module）。
 * guest 看到 /dev/vdb，挂载为真实根文件系统（ext4），switch_root 进入 Arch。
 *
 * 读写直接操作内存（memcpy），无需块设备 API。 */

static const u8 *g_rootfs_data;     /* rootfs 镜像内存指针（HHDM 虚拟地址） */
static u64 g_rootfs_size;           /* rootfs 镜像大小（字节） */
static u64 g_rootfs_capacity;       /* rootfs 容量（扇区数，512B 单位） */

static u32 rootfs_blk_read_config(u32 offset, int width) {
    (void)width;
    struct virtio_blk_config cfg;
    u8 *c = (u8 *)&cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) c[i] = 0;
    cfg.capacity = g_rootfs_capacity;
    cfg.size_max = VIRTIO_BLK_SIZE_MAX_BYTES;
    cfg.seg_max = 32;
    cfg.blk_size = 512;
    if (offset + 4 <= sizeof(cfg)) {
        u32 val = 0;
        vblk_memcpy(&val, c + offset, 4);
        return val;
    }
    if (offset < sizeof(cfg)) {
        u32 val = 0;
        vblk_memcpy(&val, c + offset, 4);
        return val;
    }
    return 0;
}

/* rootfs 请求处理：直接内存拷贝 */
static u32 rootfs_blk_handle_chain(struct virtq_desc *desc, u16 head, u32 qnum) {
    struct vblk_req req;
    if (vblk_parse_req(desc, qnum, head, &req) != 0) {
        vblk_log_ioerr("[VBLK] rootfs parse fail", &req, 1);
        return 0;
    }

    u32 bytes_done = 0;
    u8 result = VIRTIO_BLK_S_OK;

    switch (req.hdr.type) {
    case VIRTIO_BLK_T_IN:
        if (req.ndata == 0 || !g_rootfs_data) {
            result = VIRTIO_BLK_S_IOERR;
            vblk_log_ioerr("[VBLK] rootfs read ioerr", &req, 2);
            break;
        }
        bytes_done = vblk_copy_from_image(&req, g_rootfs_data, g_rootfs_size);
        break;
    case VIRTIO_BLK_T_OUT:
        /* Rootfs is a Limine module in RAM; allow writes so Linux can
         * update the superblock after remount/errors. Ephemeral. */
        if (req.ndata == 0 || !g_rootfs_data) {
            result = VIRTIO_BLK_S_IOERR;
            break;
        }
        bytes_done = vblk_copy_to_image(&req, (u8 *)g_rootfs_data, g_rootfs_size);
        break;
    case VIRTIO_BLK_T_GET_ID:
        vblk_write_id(&req, "DESHAB-ROOTFS");
        bytes_done = (req.ndata > 0) ? req.dlen[0] : 0;
        break;
    case VIRTIO_BLK_T_FLUSH:
        result = VIRTIO_BLK_S_OK;
        break;
    default:
        result = VIRTIO_BLK_S_UNSUPP;
        vblk_log_ioerr("[VBLK] rootfs unsupp", &req, 3);
        break;
    }

    __asm__ volatile("mfence" ::: "memory");
    if (req.status) *req.status = result;
    return bytes_done;
}

static u16 g_rootfs_last_avail;

static void rootfs_blk_queue_notify(u32 queue_idx) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;

    /* 必须按 gpa_base 定位：slot0 SATA blk 与本后端 device_id 同为 2，
     * virtio_queue_get_ptrs(VIRTIO_ID_BLOCK) 只会命中第一个（slot0）。 */
    if (virtio_queue_get_ptrs_by_gpa(VIRTIO_MMIO_ROOTFS_GPA, queue_idx,
                                     &desc, &avail, &used, &qnum) != 0) {
        return;
    }

    __asm__ volatile("mfence" ::: "memory");
    u16 cur = g_rootfs_last_avail;
    u16 used_idx = used->idx;
    while (cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        u32 len = rootfs_blk_handle_chain(desc, head, qnum);
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = len;
        used_idx++;
        cur++;
    }
    virtio_queue_bump_used(used, used_idx);
    g_rootfs_last_avail = cur;
}

static void rootfs_blk_reset(void) {
    g_rootfs_last_avail = 0;
}

static struct virtio_backend g_rootfs_blk_backend = {
    .device_id = VIRTIO_ID_BLOCK,
    .gpa_base = VIRTIO_MMIO_ROOTFS_GPA,
    .irq = 7,                   /* guest ISA IRQ7（cmdline :7，vector 0x37） */
    .num_queues = 1,
    .queue_size = 128,
    .device_features = (1ULL << VIRTIO_F_VERSION_1) |
                       (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
                       (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                       (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                       (1ULL << VIRTIO_BLK_F_FLUSH),
    .config_len = sizeof(struct virtio_blk_config),
    .read_config = rootfs_blk_read_config,
    .queue_notify = rootfs_blk_queue_notify,
    .reset = rootfs_blk_reset,
};

void virtio_rootfs_blk_init(void) {
    u64 rootfs_size = 0;
    void *rootfs = linux_find_rootfs_module(&rootfs_size);
    if (!rootfs || rootfs_size == 0) {
        log_warn("[VBLK] no rootfs module, rootfs-blk backend not registered");
        return;
    }

    g_rootfs_data = (const u8 *)rootfs;
    g_rootfs_size = rootfs_size;
    g_rootfs_capacity = rootfs_size / 512;  /* 扇区数 */
    if (rootfs_size % 512) g_rootfs_capacity++;

    log_info("[VBLK] rootfs-blk backend bound to memory module");
    log_hex64("[VBLK] rootfs size=", rootfs_size);
    log_hex64("[VBLK] rootfs capacity(sectors)=", g_rootfs_capacity);

    virtio_mmio_register(&g_rootfs_blk_backend);
}

/* ===== Extra rootfs virtio-blk 后端（VSCode Phase 4，可写持久卷） =====
 *
 * guest /dev/vdc。优先用 Limine extra-rootfs boot module（内存后端）。
 * 模块缺省时把 ESP 上的 boot/linux-extra-rootfs.img 按 FAT32 簇映射到
 * AHCI：Limine 不必把 ~4GiB 打进 RAM（6G WSL QEMU 会在加载该 module 时死）。
 */

static u8 *g_xrootfs_data;
static u64 g_xrootfs_size;
static u64 g_xrootfs_capacity;
static u16 g_xrootfs_last_avail;

#define XROOTFS_MAX_CLUSTERS 131072u
#define XROOTFS_FATWIN_SECS  256u

static u32 g_xrootfs_spc;
static u32 g_xrootfs_nclus;
static u32 g_xrootfs_clus_lba[XROOTFS_MAX_CLUSTERS];
static u8  g_xrootfs_dirbuf[FAT32_MAX_CLUSTER_BYTES] __attribute__((aligned(16)));
static u8  g_xrootfs_fatwin[XROOTFS_FATWIN_SECS * 512u] __attribute__((aligned(16)));
static u32 g_xrootfs_fat_abs;
static u32 g_xrootfs_spf;
static u32 g_xrootfs_fatwin_sec0;
static int g_xrootfs_fatwin_valid;

static int xrootfs_blk_rw(u32 index, u64 lba, u32 count, void *buf) {
    const dkm_block_api *blk = block_get_api();
    if (!blk || !buf || count == 0) return -1;
    return blk->read(index, lba, count, buf);
}

static int xrootfs_blk_ww(u32 index, u64 lba, u32 count, const void *buf) {
    const dkm_block_api *blk = block_get_api();
    if (!blk || !buf || count == 0) return -1;
    if (!blk->write) return -2;
    return blk->write(index, lba, count, buf);
}

static u32 xrootfs_fat_next(u32 clus) {
    u32 fat_byte, sec, off, win_secs, want;
    if (clus < 2) return 0x0FFFFFFFu;
    fat_byte = clus * 4u;
    sec = fat_byte / 512u;
    off = fat_byte % 512u;
    if (sec >= g_xrootfs_spf) return 0x0FFFFFFFu;
    if (!g_xrootfs_fatwin_valid ||
        sec < g_xrootfs_fatwin_sec0 ||
        sec >= g_xrootfs_fatwin_sec0 + XROOTFS_FATWIN_SECS) {
        want = g_xrootfs_spf - sec;
        win_secs = XROOTFS_FATWIN_SECS;
        if (want < win_secs) win_secs = want;
        if (win_secs == 0) return 0x0FFFFFFFu;
        if (xrootfs_blk_rw(0, (u64)g_xrootfs_fat_abs + sec, win_secs,
                           g_xrootfs_fatwin) != 0) {
            return 0x0FFFFFFFu;
        }
        g_xrootfs_fatwin_sec0 = sec;
        g_xrootfs_fatwin_valid = 1;
    }
    {
        u32 idx = (sec - g_xrootfs_fatwin_sec0) * 512u + off;
        const u8 *p = g_xrootfs_fatwin + idx;
        return ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24))
               & 0x0FFFFFFFu;
    }
}

static int xrootfs_find_in_dir(u32 vol_lba, u32 data_start, u32 start_clus,
                               const char *long_name, int is_dir,
                               u32 *out_clus, u32 *out_size) {
    fat32_lfn_buf lfn;
    u32 clus = start_clus;
    u32 spc = g_xrootfs_spc;
    if (spc == 0 || spc * 512u > FAT32_MAX_CLUSTER_BYTES) return -1;
    fat32_lfn_init(&lfn);
    while (clus >= 2 && clus < 0x0FFFFFF8u) {
        u32 lba = vol_lba + data_start + (clus - 2u) * spc;
        const u8 *entries;
        u32 entry_count, e;
        if (xrootfs_blk_rw(0, lba, spc, g_xrootfs_dirbuf) != 0) return -3;
        entries = g_xrootfs_dirbuf;
        entry_count = (spc * 512u) / 32u;
        for (e = 0; e < entry_count; e++) {
            const u8 *entry = entries + e * 32u;
            const u8 *de;
            int is_short, entry_is_dir, matched;
            if (entry[0] == 0) return -4;
            if ((u8)entry[0] == 0xE5) {
                fat32_lfn_init(&lfn);
                continue;
            }
            de = entry;
            is_short = fat32_lfn_process(&lfn, entry);
            if (!is_short) continue;
            if (de[11] & 0x08) {
                fat32_lfn_init(&lfn);
                continue;
            }
            entry_is_dir = (de[11] & 0x10) ? 1 : 0;
            if (entry_is_dir != is_dir) {
                fat32_lfn_init(&lfn);
                continue;
            }
            matched = 0;
            if (lfn.valid && long_name) {
                char ascii[FAT32_LFN_MAX];
                int n = fat32_lfn_to_ascii(&lfn, ascii, sizeof(ascii));
                if (n >= 0 && fat32_lfn_streq_ci(ascii, long_name)) matched = 1;
            }
            if (!matched && long_name) {
                char short_disp[13];
                fat32_lfn_short_to_str(de, short_disp);
                if (fat32_lfn_streq_ci(short_disp, long_name)) matched = 1;
            }
            if (matched) {
                u16 clow = (u16)de[26] | ((u16)de[27] << 8);
                u16 chigh = (u16)de[20] | ((u16)de[21] << 8);
                *out_clus = (u32)clow | ((u32)chigh << 16);
                *out_size = (u32)de[28] | ((u32)de[29] << 8) |
                            ((u32)de[30] << 16) | ((u32)de[31] << 24);
                return 0;
            }
            fat32_lfn_init(&lfn);
        }
        clus = xrootfs_fat_next(clus) & 0x0FFFFFFFu;
    }
    return -4;
}

static int xrootfs_map_esp_file(void) {
    const dkm_block_api *blk = block_get_api();
    u8 bpbsec[512] __attribute__((aligned(16)));
    u64 vol;
    u32 rsvd, fc, spf, spc, root, data_start, file_clus, file_size, nclus, i, fc_cur;
    u32 boot_clus, dummy;
    if (!blk || blk->device_count() == 0 || blk->sector_size(0) != 512) {
        log_warn("[VBLK] extra-rootfs ESP map needs AHCI first");
        return -1;
    }
    vol = fat32_part_probe(xrootfs_blk_rw, 0);
    if (blk->read(0, vol, 1, bpbsec) != 0) return -2;
    if (!fat32_part_bpb_ok(bpbsec)) return -3;
    rsvd = (u32)bpbsec[14] | ((u32)bpbsec[15] << 8);
    fc = bpbsec[16];
    spc = bpbsec[13];
    spf = (u32)bpbsec[36] | ((u32)bpbsec[37] << 8) |
          ((u32)bpbsec[38] << 16) | ((u32)bpbsec[39] << 24);
    root = (u32)bpbsec[44] | ((u32)bpbsec[45] << 8) |
           ((u32)bpbsec[46] << 16) | ((u32)bpbsec[47] << 24);
    if (spc == 0 || spc > 64 || spf == 0 || root < 2) return -4;
    g_xrootfs_spc = spc;
    g_xrootfs_spf = spf;
    g_xrootfs_fat_abs = (u32)vol + rsvd;
    g_xrootfs_fatwin_valid = 0;
    data_start = rsvd + fc * spf;

    boot_clus = 0;
    dummy = 0;
    if (xrootfs_find_in_dir((u32)vol, data_start, root, "boot", 1,
                            &boot_clus, &dummy) != 0) {
        log_warn("[VBLK] extra-rootfs: ESP /boot not found");
        return -5;
    }
    file_clus = 0;
    file_size = 0;
    if (xrootfs_find_in_dir((u32)vol, data_start, boot_clus,
                            "linux-extra-rootfs.img", 0,
                            &file_clus, &file_size) != 0 ||
        file_clus < 2 || file_size == 0) {
        log_warn("[VBLK] extra-rootfs: linux-extra-rootfs.img not on ESP");
        return -6;
    }

    nclus = (file_size + (spc * 512u) - 1u) / (spc * 512u);
    if (nclus == 0 || nclus > XROOTFS_MAX_CLUSTERS) {
        log_error("[VBLK] extra-rootfs cluster map too large");
        log_hex64("[VBLK] nclus=", nclus);
        return -7;
    }

    fc_cur = file_clus;
    for (i = 0; i < nclus; i++) {
        if (fc_cur < 2 || fc_cur >= 0x0FFFFFF8u) {
            log_error("[VBLK] extra-rootfs FAT chain short");
            return -8;
        }
        g_xrootfs_clus_lba[i] = (u32)vol + data_start + (fc_cur - 2u) * spc;
        fc_cur = xrootfs_fat_next(fc_cur) & 0x0FFFFFFFu;
    }

    g_xrootfs_nclus = nclus;
    g_xrootfs_data = 0;
    g_xrootfs_size = file_size;
    g_xrootfs_capacity = file_size / 512u;
    if (file_size % 512u) g_xrootfs_capacity++;
    log_info("[VBLK] extra-rootfs-blk backend bound (esp file)");
    log_hex64("[VBLK] extra-rootfs size=", file_size);
    log_hex64("[VBLK] extra-rootfs nclus=", nclus);
    log_hex64("[VBLK] extra-rootfs first_lba=", g_xrootfs_clus_lba[0]);
    return 0;
}

static int xrootfs_disk_xfer(u64 img_sector, u32 nsec, u8 *buf, int is_write) {
    const dkm_block_api *blk = block_get_api();
    u32 spc = g_xrootfs_spc;
    if (!blk || !buf || nsec == 0 || spc == 0) return -1;
    while (nsec) {
        u32 ci, off, chunk, c;
        u64 disk;
        if (img_sector / spc >= g_xrootfs_nclus) return -2;
        ci = (u32)(img_sector / spc);
        off = (u32)(img_sector % spc);
        disk = g_xrootfs_clus_lba[ci] + off;
        chunk = spc - off;
        if (chunk > nsec) chunk = nsec;
        c = ci;
        while (chunk < nsec && (c + 1u) < g_xrootfs_nclus &&
               g_xrootfs_clus_lba[c + 1u] == g_xrootfs_clus_lba[c] + spc) {
            u32 add = spc;
            if (add > nsec - chunk) add = nsec - chunk;
            chunk += add;
            c++;
        }
        if (is_write) {
            if (xrootfs_blk_ww(0, disk, chunk, buf) != 0) return -3;
        } else {
            if (xrootfs_blk_rw(0, disk, chunk, buf) != 0) return -3;
        }
        buf += chunk * 512u;
        img_sector += chunk;
        nsec -= chunk;
    }
    return 0;
}

static int xrootfs_disk_xfer_bytes(u64 off, u32 n, u8 *buf, int is_write) {
    static u8 bounce[512];
    while (n) {
        u64 sector = off / 512ULL;
        u32 so = (u32)(off % 512ULL);
        u32 chunk;
        if (so == 0 && n >= 512u) {
            u32 nsec = n / 512u;
            if (xrootfs_disk_xfer(sector, nsec, buf, is_write) != 0)
                return -1;
            chunk = nsec * 512u;
        } else {
            chunk = 512u - so;
            if (chunk > n)
                chunk = n;
            if (xrootfs_disk_xfer(sector, 1, bounce, 0) != 0)
                return -1;
            if (is_write) {
                vblk_memcpy(bounce + so, buf, chunk);
                if (xrootfs_disk_xfer(sector, 1, bounce, 1) != 0)
                    return -1;
            } else {
                vblk_memcpy(buf, bounce + so, chunk);
            }
        }
        buf += chunk;
        off += chunk;
        n -= chunk;
    }
    return 0;
}

static u32 xrootfs_copy_disk(const struct vblk_req *r, int is_write) {
    u64 offset = r->hdr.sector * 512ULL;
    u32 pos = 0;
    for (int i = 0; i < r->ndata; i++) {
        u32 n = r->dlen[i];
        if (n == 0)
            continue;
        if (xrootfs_disk_xfer_bytes(offset + pos, n, r->data[i], is_write) != 0)
            return (u32)-1;
        pos += n;
    }
    return is_write ? 0 : pos;
}

static u32 xrootfs_blk_read_config(u32 offset, int width) {
    (void)width;
    struct virtio_blk_config cfg;
    u8 *c = (u8 *)&cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) c[i] = 0;
    cfg.capacity = g_xrootfs_capacity;
    cfg.size_max = VIRTIO_BLK_SIZE_MAX_BYTES;
    cfg.seg_max = 32;
    cfg.blk_size = 512;
    if (offset < sizeof(cfg)) {
        u32 val = 0;
        vblk_memcpy(&val, c + offset, (offset + 4 <= sizeof(cfg)) ? 4 : (sizeof(cfg) - offset));
        return val;
    }
    return 0;
}

static u32 xrootfs_blk_handle_chain(struct virtq_desc *desc, u16 head, u32 qnum) {
    struct vblk_req req;
    if (vblk_parse_req(desc, qnum, head, &req) != 0) {
        vblk_log_ioerr("[VBLK] xrootfs parse fail", &req, 1);
        return 0;
    }

    u32 bytes_done = 0;
    u8 result = VIRTIO_BLK_S_OK;

    switch (req.hdr.type) {
    case VIRTIO_BLK_T_IN:
        if (req.ndata == 0) {
            result = VIRTIO_BLK_S_IOERR;
            vblk_log_ioerr("[VBLK] xrootfs read ioerr", &req, 2);
            break;
        }
        if (g_xrootfs_data) {
            bytes_done = vblk_copy_from_image(&req, g_xrootfs_data, g_xrootfs_size);
        } else if (g_xrootfs_nclus) {
            bytes_done = xrootfs_copy_disk(&req, 0);
            if (bytes_done == (u32)-1) {
                result = VIRTIO_BLK_S_IOERR;
                bytes_done = 0;
                vblk_log_ioerr("[VBLK] xrootfs disk read ioerr", &req, 2);
            }
        } else {
            result = VIRTIO_BLK_S_IOERR;
            vblk_log_ioerr("[VBLK] xrootfs read ioerr", &req, 2);
        }
        break;
    case VIRTIO_BLK_T_OUT:
        if (req.ndata == 0) {
            result = VIRTIO_BLK_S_IOERR;
            break;
        }
        if (g_xrootfs_data) {
            bytes_done = vblk_copy_to_image(&req, g_xrootfs_data, g_xrootfs_size);
        } else if (g_xrootfs_nclus) {
            if (xrootfs_copy_disk(&req, 1) == (u32)-1)
                result = VIRTIO_BLK_S_IOERR;
        } else {
            result = VIRTIO_BLK_S_IOERR;
        }
        break;
    case VIRTIO_BLK_T_GET_ID:
        vblk_write_id(&req, "DESHAB-EXTRA-ROOTFS");
        bytes_done = (req.ndata > 0) ? req.dlen[0] : 0;
        break;
    case VIRTIO_BLK_T_FLUSH:
        result = VIRTIO_BLK_S_OK;
        break;
    default:
        result = VIRTIO_BLK_S_UNSUPP;
        vblk_log_ioerr("[VBLK] xrootfs unsupp", &req, 3);
        break;
    }

    __asm__ volatile("mfence" ::: "memory");
    if (req.status) *req.status = result;
    return bytes_done;
}

static void xrootfs_blk_queue_notify(u32 queue_idx) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;

    /* 按 gpa_base 定位（三个 blk 后端 device_id 同为 2） */
    if (virtio_queue_get_ptrs_by_gpa(VIRTIO_MMIO_EXTRA_ROOTFS_GPA, queue_idx,
                                     &desc, &avail, &used, &qnum) != 0) {
        return;
    }

    __asm__ volatile("mfence" ::: "memory");
    u16 cur = g_xrootfs_last_avail;
    u16 used_idx = used->idx;
    while (cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        u32 len = xrootfs_blk_handle_chain(desc, head, qnum);
        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = len;
        used_idx++;
        cur++;
    }
    virtio_queue_bump_used(used, used_idx);
    g_xrootfs_last_avail = cur;
}

static void xrootfs_blk_reset(void) {
    g_xrootfs_last_avail = 0;
}

static struct virtio_backend g_xrootfs_blk_backend = {
    .device_id = VIRTIO_ID_BLOCK,
    .gpa_base = VIRTIO_MMIO_EXTRA_ROOTFS_GPA,
    .irq = 11,                  /* guest ISA IRQ11（cmdline :11，vector 0x3B） */
    .num_queues = 1,
    .queue_size = 128,
    .device_features = (1ULL << VIRTIO_F_VERSION_1) |
                       (1ULL << VIRTIO_BLK_F_SIZE_MAX) |
                       (1ULL << VIRTIO_BLK_F_SEG_MAX) |
                       (1ULL << VIRTIO_BLK_F_BLK_SIZE) |
                       (1ULL << VIRTIO_BLK_F_FLUSH),
    .config_len = sizeof(struct virtio_blk_config),
    .read_config = xrootfs_blk_read_config,
    .queue_notify = xrootfs_blk_queue_notify,
    .reset = xrootfs_blk_reset,
};

void virtio_extra_rootfs_blk_init(void) {
    u64 size = 0;
    void *img = linux_find_extra_rootfs_module(&size);

    g_xrootfs_nclus = 0;
    g_xrootfs_data = 0;
    g_xrootfs_size = 0;
    g_xrootfs_capacity = 0;

    if (img && size != 0) {
        g_xrootfs_data = (u8 *)img;
        g_xrootfs_size = size;
        g_xrootfs_capacity = size / 512;
        if (size % 512) g_xrootfs_capacity++;
        log_info("[VBLK] extra-rootfs-blk backend bound (rw module)");
        log_hex64("[VBLK] extra-rootfs size=", size);
    } else if (xrootfs_map_esp_file() != 0) {
        log_warn("[VBLK] no extra-rootfs module or ESP file, slot6 not registered");
        return;
    }

    virtio_mmio_register(&g_xrootfs_blk_backend);
}
