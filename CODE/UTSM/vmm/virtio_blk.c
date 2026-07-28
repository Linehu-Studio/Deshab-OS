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

/* device config 空间（VIRTIO_MMIO_CONFIG 起） */
struct virtio_blk_config {
    u64 capacity;       /* 总扇区数（512B） */
    u32 size_max;
    u32 seg_max;
    u16 geometry[4];
    u32 blk_size;
    /* ... 其余字段省略 */
} __attribute__((packed));

/* 后端状态 */
static u64 g_blk_capacity;      /* 扇区数 */
static u32 g_blk_sector_size = 512;
static int g_blk_bound = -1;    /* 绑定的 UTSM block 设备索引 */

/* 读取 device config 空间 */
static u32 blk_read_config(u32 offset, int width) {
    (void)width;
    struct virtio_blk_config cfg;
    /* 填零后逐字段读取 */
    u8 *c = (u8 *)&cfg;
    for (u32 i = 0; i < sizeof(cfg); i++) c[i] = 0;
    cfg.capacity = g_blk_capacity;
    cfg.size_max = 128;                 /* 单请求最大 128 扇区 */
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
    (void)qnum;
    if (!desc) return 0;

    /* 第一个描述符：请求头（driver→device，16 字节） */
    struct virtio_blk_outhdr *hdr =
        (struct virtio_blk_outhdr *)virtio_gpa_to_host(desc[head].addr);
    if (!hdr) return 0;

    u32 type = hdr->type;
    u64 sector = hdr->sector;

    /* 沿描述符链找到 data 与 status 描述符 */
    u16 cur = head;
    u16 data_idx = 0xFFFF, status_idx = 0xFFFF;
    int hops = 0;
    if (desc[cur].flags & VIRTQ_DESC_F_NEXT) {
        data_idx = desc[cur].next;
        cur = data_idx;
        /* data 之后可能还有 status */
        while ((desc[cur].flags & VIRTQ_DESC_F_NEXT) && hops < 16) {
            cur = desc[cur].next;
            hops++;
        }
        /* 最后一个是 status（device write，1 字节） */
        status_idx = cur;
    }

    u8 *status = (u8 *)0;
    if (status_idx != 0xFFFF && (desc[status_idx].flags & VIRTQ_DESC_F_WRITE)) {
        status = (u8 *)virtio_gpa_to_host(desc[status_idx].addr);
    }

    u32 bytes_done = 0;
    u8 result = VIRTIO_BLK_S_OK;

    const dkm_block_api *blk = block_get_api();

    switch (type) {
    case VIRTIO_BLK_T_IN: {  /* 读：块设备 → guest data buffer */
        if (data_idx == 0xFFFF || g_blk_bound < 0 || !blk) {
            result = VIRTIO_BLK_S_IOERR;
            break;
        }
        u8 *dbuf = (u8 *)virtio_gpa_to_host(desc[data_idx].addr);
        u32 dlen = desc[data_idx].len;
        if (!dbuf) { result = VIRTIO_BLK_S_IOERR; break; }
        u32 nsec = dlen / 512;
        if (nsec == 0) nsec = 1;
        if (blk->read((u32)g_blk_bound, sector, nsec, dbuf) != 0) {
            result = VIRTIO_BLK_S_IOERR;
        } else {
            bytes_done = dlen;
        }
        break;
    }
    case VIRTIO_BLK_T_OUT: {  /* 写：guest data buffer → 块设备 */
        if (data_idx == 0xFFFF || g_blk_bound < 0 || !blk) {
            result = VIRTIO_BLK_S_IOERR;
            break;
        }
        const u8 *dbuf = (const u8 *)virtio_gpa_to_host(desc[data_idx].addr);
        u32 dlen = desc[data_idx].len;
        if (!dbuf) { result = VIRTIO_BLK_S_IOERR; break; }
        u32 nsec = dlen / 512;
        if (nsec == 0) nsec = 1;
        if (blk->write((u32)g_blk_bound, sector, nsec, dbuf) != 0) {
            result = VIRTIO_BLK_S_IOERR;
        } else {
            bytes_done = 0;  /* 写操作 used.len=0 */
        }
        break;
    }
    case VIRTIO_BLK_T_GET_ID: {
        if (data_idx != 0xFFFF) {
            char *idbuf = (char *)virtio_gpa_to_host(desc[data_idx].addr);
            if (idbuf) {
                const char *id = "DESHAB-VIRTIO-BLK";
                u32 i = 0;
                for (; id[i] && i < desc[data_idx].len - 1; i++) idbuf[i] = id[i];
                idbuf[i] = 0;
                bytes_done = i + 1;
            }
        }
        break;
    }
    case VIRTIO_BLK_T_FLUSH:
        result = VIRTIO_BLK_S_OK;  /* 无写缓存，直接成功 */
        break;
    default:
        result = VIRTIO_BLK_S_UNSUPP;
        break;
    }

    /* 写状态字节 */
    if (status) *status = result;
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
         * 实际 capacity 需读 ATA IDENTIFY；这里给 4GB 占位，
         * guest mkfs/mount 会按此上限工作。 */
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
