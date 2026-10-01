/* virtio_mmio.c — UTSM virtio-mmio 设备模拟后端框架实现。
 *
 * 管理一组 virtio_backend 设备（block + net），处理 guest 对 MMIO 寄存器
 * 的读/写。寄存器语义遵循 Virtio Spec 1.1 §4.2（modern，version=2）。
 *
 * guest 设备发现：cmdline virtio_mmio.device=SIZE@GPA:IRQ 参数（见
 * linux_loader 的 cmdline 注入）。QUEUE_NOTIFY 处理完后经
 * vmx_guest_queue_irq 注入虚拟中断（legacy PIC，vector=0x30+irq）。
 */

#include <utsm/virtio_mmio.h>
#include <utsm/linux_loader.h>
#include <utsm/ept.h>
#include <utsm/vmx.h>
#include <utsm/dma.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

#define VIRTIO_MMIO_MAGIC 0x74726976   /* "virt" */
#define VIRTIO_MMIO_VERSION_MODERN 2
/* VSCode Phase 1: 3 (blk/net/rootfs) → 8 slots for input/gpu/extra-rootfs. */
#define MAX_VIRTIO_DEVICES 8

/* 每个 virtio 设备的运行状态（框架层维护，后端无关） */
struct virtio_dev_state {
    struct virtio_backend *be;      /* 后端驱动（NULL = 未注册） */
    u32 status;                     /* device status */
    u32 drv_features_sel;           /* DRIVER_FEATURES_SEL */
    u32 dev_features_sel;           /* DEVICE_FEATURES_SEL */
    u64 driver_features;            /* guest 协商的 features */
    u32 queue_sel;                  /* 当前选中队列 */
    u32 interrupt_status;           /* INTERRUPT_STATUS */
    u32 config_generation;

    /* 每队列状态 */
    struct {
        u32 num;                    /* QUEUE_NUM（实际启用大小） */
        u32 ready;                  /* QUEUE_READY */
        u64 desc_gpa;               /* 描述符表 GPA */
        u64 driver_gpa;             /* avail ring GPA */
        u64 device_gpa;             /* used ring GPA */
    } q[4];
};

static struct virtio_dev_state g_vdevs[MAX_VIRTIO_DEVICES];
static int g_vdev_count;
static int g_vio_ready;

/* ===== guest 内存访问辅助 ===== */

void *virtio_gpa_to_host(u64 gpa) {
    u64 hpa = ept_gpa_to_hpa(gpa);
    /* Guest CPU stores fault in and demand-map a page. virtio-blk READ
     * buffers are often allocated but never stored by the vCPU first, so
     * the EPT walk misses and the device must map the page itself.
     * Skip GPA 0 (null) and MMIO (>= 0xF0000000). */
    if (hpa == 0 && gpa >= 0x1000ULL && gpa < 0xF0000000ULL) {
        u64 page = gpa & ~(EPT_PAGE_SIZE - 1);
        static u32 demand_logs;
        dkm_dma_buffer buf;
        if (dma_alloc_pages(1, EPT_PAGE_SIZE, 0, &buf) == 0 &&
            ept_map_range(page, buf.phys, EPT_PAGE_SIZE, EPT_RWX) == 0) {
            hpa = buf.phys + (gpa & (EPT_PAGE_SIZE - 1));
            if (demand_logs < 8) {
                demand_logs++;
                log_hex64("[VIO] demand-map gpa=", gpa);
            }
        }
    }
    if (hpa == 0) return (void *)0;
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    return (void *)(hhdm + hpa);
}

/* ===== 设备注册 ===== */

int virtio_mmio_register(struct virtio_backend *dev) {
    if (!dev || g_vdev_count >= MAX_VIRTIO_DEVICES) return -1;
    struct virtio_dev_state *s = &g_vdevs[g_vdev_count];
    s->be = dev;
    s->status = 0;
    s->drv_features_sel = 0;
    s->dev_features_sel = 0;
    s->driver_features = 0;
    s->queue_sel = 0;
    s->interrupt_status = 0;
    s->config_generation = 0;
    for (int i = 0; i < 4; i++) {
        s->q[i].num = 0;
        s->q[i].ready = 0;
        s->q[i].desc_gpa = 0;
        s->q[i].driver_gpa = 0;
        s->q[i].device_gpa = 0;
    }
    g_vdev_count++;
    log_hex64("[VIO] registered device id=", dev->device_id);
    log_hex64("[VIO]   gpa=", dev->gpa_base);
    return 0;
}

/* 按 GPA 查找设备（含该设备的 4KB 寄存器区）。 */
static struct virtio_dev_state *find_dev(u64 gpa, u32 *offset_out) {
    for (int i = 0; i < g_vdev_count; i++) {
        u64 base = g_vdevs[i].be->gpa_base;
        if (gpa >= base && gpa < base + 0x1000) {
            if (offset_out) *offset_out = (u32)(gpa - base);
            return &g_vdevs[i];
        }
    }
    return (struct virtio_dev_state *)0;
}

/* ===== feature 读取（按 sel 返回 32 位窗口） ===== */

static u32 read_features(struct virtio_dev_state *s) {
    if (s->dev_features_sel == 0) {
        return (u32)(s->be->device_features & 0xFFFFFFFFULL);
    } else if (s->dev_features_sel == 1) {
        return (u32)(s->be->device_features >> 32);
    }
    return 0;
}

/* ===== 寄存器读 ===== */

u64 virtio_mmio_read(u64 gpa, int width) {
    (void)width;  /* virtio-mmio 寄存器固定 32 位访问 */
    u32 off;
    struct virtio_dev_state *s = find_dev(gpa, &off);
    if (!s || !s->be) {
        log_hex64("[VIO] read unmapped gpa=", gpa);
        return 0;
    }

    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE:    return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:        return VIRTIO_MMIO_VERSION_MODERN;
    case VIRTIO_MMIO_DEVICE_ID:      return s->be->device_id;
    case VIRTIO_MMIO_VENDOR_ID:      return 0x55444D55;  /* "UMDU" 自定义 */
    case VIRTIO_MMIO_DEVICE_FEATURES:return read_features(s);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:  return s->be->queue_size;
    case VIRTIO_MMIO_QUEUE_READY:    return s->q[s->queue_sel].ready;
    case VIRTIO_MMIO_INTERRUPT_STATUS:return s->interrupt_status;
    case VIRTIO_MMIO_STATUS:         return s->status;
    case VIRTIO_MMIO_SHM_LEN_LOW:
    case VIRTIO_MMIO_SHM_LEN_HIGH:
        /* No SHM windows. Linux treats (u64)-1 as "region absent". */
        return 0xFFFFFFFFu;
    case VIRTIO_MMIO_SHM_BASE_LOW:
    case VIRTIO_MMIO_SHM_BASE_HIGH:
        return 0;
    case VIRTIO_MMIO_CONFIG_GENERATION:return s->config_generation;
    default:
        /* device config 空间（0x100 起） */
        if (off >= VIRTIO_MMIO_CONFIG && off < VIRTIO_MMIO_CONFIG + s->be->config_len) {
            if (s->be->read_config) {
                return s->be->read_config(off - VIRTIO_MMIO_CONFIG, width);
            }
        }
        return 0;
    }
}

/* ===== 寄存器写 ===== */

void virtio_mmio_write(u64 gpa, u64 value, int width) {
    (void)width;
    u32 off;
    struct virtio_dev_state *s = find_dev(gpa, &off);
    if (!s || !s->be) {
        log_hex64("[VIO] write unmapped gpa=", gpa);
        return;
    }
    u32 v32 = (u32)value;

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        s->dev_features_sel = v32;
        break;
    case VIRTIO_MMIO_SHM_SEL:
        /* No SHM regions; selector is ignored. Length reads stay -1. */
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if (s->drv_features_sel == 0) {
            s->driver_features = (s->driver_features & 0xFFFFFFFF00000000ULL) | v32;
        } else if (s->drv_features_sel == 1) {
            s->driver_features = (s->driver_features & 0xFFFFFFFFULL) | ((u64)v32 << 32);
        }
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        s->drv_features_sel = v32;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        if (v32 < 4) s->queue_sel = v32;
        break;
    case VIRTIO_MMIO_QUEUE_NUM: {
        /* 限制在后端 queue_size 内 */
        if (v32 == 0 || v32 > s->be->queue_size) {
            s->q[s->queue_sel].num = s->be->queue_size;
        } else {
            s->q[s->queue_sel].num = v32;
        }
        break;
    }
    case VIRTIO_MMIO_QUEUE_READY:
        s->q[s->queue_sel].ready = v32 ? 1 : 0;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        s->q[s->queue_sel].desc_gpa = (s->q[s->queue_sel].desc_gpa & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        s->q[s->queue_sel].desc_gpa = (s->q[s->queue_sel].desc_gpa & 0xFFFFFFFFULL) | ((u64)v32 << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        s->q[s->queue_sel].driver_gpa = (s->q[s->queue_sel].driver_gpa & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        s->q[s->queue_sel].driver_gpa = (s->q[s->queue_sel].driver_gpa & 0xFFFFFFFFULL) | ((u64)v32 << 32);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        s->q[s->queue_sel].device_gpa = (s->q[s->queue_sel].device_gpa & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        s->q[s->queue_sel].device_gpa = (s->q[s->queue_sel].device_gpa & 0xFFFFFFFFULL) | ((u64)v32 << 32);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        /* guest 通知设备处理队列 v32。调用后端处理请求。 */
        if (s->be->queue_notify) {
            s->be->queue_notify(v32);
        }
        /* 设置中断状态位（used buffer notification）并注入虚拟 IRQ。
         * legacy PIC 模式：vector = 0x30 + irq（master PIC 基址 0x30）。
         * 是否真正投递由 vmexit 的 maybe_inject_irq 按 IMR/IF 决定。 */
        s->interrupt_status |= 1;
        if (s->be->irq != 0) {
            vmx_guest_queue_irq(0x30 + s->be->irq);
        }
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        s->interrupt_status &= ~v32;
        break;
    case VIRTIO_MMIO_STATUS:
        s->status = v32;
        if (v32 == 0) {
            /* 复位 */
            if (s->be->reset) s->be->reset();
            s->driver_features = 0;
            s->interrupt_status = 0;
            for (int i = 0; i < 4; i++) {
                s->q[i].ready = 0;
                s->q[i].num = 0;
            }
        }
        break;
    default:
        /* VSCode Phase 2: device config 空间写。virtio-input 用此通道设置
         * select/subsel 后再读结果；其它设备 config 只读（write_config=NULL）。 */
        if (off >= VIRTIO_MMIO_CONFIG && off < VIRTIO_MMIO_CONFIG + s->be->config_len) {
            if (s->be->write_config) {
                s->be->write_config(off - VIRTIO_MMIO_CONFIG, v32);
            }
        }
        break;
    }
}

/* ===== 队列访问 API 实现 =====
 *
 * 后端 queue_notify 调用 virtio_queue_get_ptrs 取队列三个表的 host 地址，
 * 自行遍历 avail ring 处理请求，写 used ring 回执。 */

int virtio_queue_get_ptrs(u32 device_id, u32 queue_idx,
                          struct virtq_desc **desc_out,
                          struct virtq_avail **avail_out,
                          struct virtq_used **used_out,
                          u32 *queue_num_out) {
    struct virtio_dev_state *s = (struct virtio_dev_state *)0;
    for (int i = 0; i < g_vdev_count; i++) {
        if (g_vdevs[i].be && g_vdevs[i].be->device_id == device_id) {
            s = &g_vdevs[i];
            break;
        }
    }
    if (!s || queue_idx >= 4) return -1;
    if (!s->q[queue_idx].ready) return -2;

    struct virtq_desc *desc = (struct virtq_desc *)virtio_gpa_to_host(s->q[queue_idx].desc_gpa);
    struct virtq_avail *avail = (struct virtq_avail *)virtio_gpa_to_host(s->q[queue_idx].driver_gpa);
    struct virtq_used *used = (struct virtq_used *)virtio_gpa_to_host(s->q[queue_idx].device_gpa);
    if (!desc || !avail || !used) return -3;

    if (desc_out) *desc_out = desc;
    if (avail_out) *avail_out = avail;
    if (used_out) *used_out = used;
    if (queue_num_out) *queue_num_out = s->q[queue_idx].num ? s->q[queue_idx].num : s->be->queue_size;
    return 0;
}

/* VSCode Phase 2: 按 gpa_base 定位队列。用于两个 virtio-input（kbd+mouse），
 * 它们 device_id 相同（VIRTIO_ID_INPUT=18），只能靠 gpa_base 区分。 */
int virtio_queue_get_ptrs_by_gpa(u64 gpa_base, u32 queue_idx,
                                 struct virtq_desc **desc_out,
                                 struct virtq_avail **avail_out,
                                 struct virtq_used **used_out,
                                 u32 *queue_num_out) {
    struct virtio_dev_state *s = (struct virtio_dev_state *)0;
    for (int i = 0; i < g_vdev_count; i++) {
        if (g_vdevs[i].be && g_vdevs[i].be->gpa_base == gpa_base) {
            s = &g_vdevs[i];
            break;
        }
    }
    if (!s || queue_idx >= 4) return -1;
    if (!s->q[queue_idx].ready) return -2;

    struct virtq_desc *desc = (struct virtq_desc *)virtio_gpa_to_host(s->q[queue_idx].desc_gpa);
    struct virtq_avail *avail = (struct virtq_avail *)virtio_gpa_to_host(s->q[queue_idx].driver_gpa);
    struct virtq_used *used = (struct virtq_used *)virtio_gpa_to_host(s->q[queue_idx].device_gpa);
    if (!desc || !avail || !used) return -3;

    if (desc_out) *desc_out = desc;
    if (avail_out) *avail_out = avail;
    if (used_out) *used_out = used;
    if (queue_num_out) *queue_num_out = s->q[queue_idx].num ? s->q[queue_idx].num : s->be->queue_size;
    return 0;
}

void virtio_queue_bump_used(struct virtq_used *used, u16 new_idx) {
    if (!used) return;
    __asm__ volatile("mfence" ::: "memory");
    used->idx = new_idx;
    __asm__ volatile("mfence" ::: "memory");
}

void virtio_mmio_raise_irq(u32 device_id) {
    for (int i = 0; i < g_vdev_count; i++) {
        struct virtio_dev_state *s = &g_vdevs[i];
        if (!s->be || s->be->device_id != device_id) continue;
        s->interrupt_status |= 1;
        if (s->be->irq != 0) {
            vmx_guest_queue_irq(0x30 + s->be->irq);
        }
        return;
    }
}

/* VSCode Phase 2: 按 gpa_base 触发中断（两个 virtio-input 同 device_id=18
 * 但不同 gpa_base / irq，需要分别注入）。 */
void virtio_mmio_raise_irq_by_gpa(u64 gpa_base) {
    for (int i = 0; i < g_vdev_count; i++) {
        struct virtio_dev_state *s = &g_vdevs[i];
        if (!s->be || s->be->gpa_base != gpa_base) continue;
        s->interrupt_status |= 1;
        if (s->be->irq != 0) {
            vmx_guest_queue_irq(0x30 + s->be->irq);
        }
        return;
    }
}

/* ===== 框架初始化 =====
 *
 * 注册 block 与 net 后端。后端结构体由 virtio_blk.c / virtio_net.c 提供。 */
extern struct virtio_backend g_virtio_blk_backend;
extern struct virtio_backend g_virtio_net_backend;

/* 后端初始化符号（定义于 virtio_blk.c / virtio_net.c / virtio_input.c /
 * virtio_gpu.c）。这里声明以便链接；具体注册在各自文件完成。 */
void virtio_blk_backend_init(void);
void virtio_net_backend_init(void);
void virtio_rootfs_blk_init(void);  /* memory-backed Arch rootfs (/dev/vdb) */
void virtio_input_backend_init(void);  /* VSCode Phase 2: keyboard + mouse */
void virtio_gpu_backend_init(void);     /* VSCode Phase 3: virtio-gpu 2D */
void virtio_extra_rootfs_blk_init(void); /* VSCode Phase 4: rw volume (/dev/vdc) */

void virtio_mmio_init(void) {
    if (g_vio_ready) return;
    g_vdev_count = 0;

    virtio_blk_backend_init();
    virtio_net_backend_init();
    /* Register memory-backed rootfs backend (no-op if no rootfs module found).
     * Must come after blk/net so /dev/vda (SATA) and /dev/vdb (rootfs) order
     * is deterministic. */
    virtio_rootfs_blk_init();
    /* VSCode Phase 2: virtio-input (keyboard + mouse). Safe no-op until the
     * desktop enables input forwarding via the push_* API. */
    virtio_input_backend_init();
    /* VSCode Phase 3: virtio-gpu 2D (scanout compositing into surface pool).
     * Safe no-op if the surface pool was not allocated (Linux guest not loaded
     * or alloc failed) — scanout stays disabled. */
    virtio_gpu_backend_init();
    /* VSCode Phase 4: writable extra-rootfs volume (/dev/vdc, overlay upper).
     * Safe no-op if no extra-rootfs boot module — guest probe sees bad magic
     * on slot6 and skips it. */
    virtio_extra_rootfs_blk_init();

    g_vio_ready = 1;
    log_info("[VIO] virtio-mmio backend init ok");
}
