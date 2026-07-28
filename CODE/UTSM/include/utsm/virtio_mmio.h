#ifndef UTSM_VIRTIO_MMIO_H
#define UTSM_VIRTIO_MMIO_H

/* virtio_mmio.h — UTSM virtio-mmio 设备模拟后端框架。
 *
 * 为 Linux guest 提供 virtio-mmio 设备（block + net）。
 * 设备 GPA 位于 VIRTIO_MMIO_GPA_BASE 起的高位 MMIO 区，EPT 故意不映射，
 * guest 访问时触发 EPT violation，vmexit handler 调用本模块的
 * virtio_mmio_read/write 模拟寄存器行为。
 *
 * 参考 Virtio Spec 1.1 §4.2 (virtio-mmio)。
 * guest 通过 cmdline virtio_mmio.device= 参数发现设备（无需 PCI/ACPI）。
 */

#include <utsm/types.h>

/* virtio-mmio 寄存器偏移（Virtio Spec 1.1 §4.2.2.1，均为 32 位寄存器） */
#define VIRTIO_MMIO_MAGIC_VALUE      0x000   /* R  "virt" = 0x74726976 */
#define VIRTIO_MMIO_VERSION          0x004   /* R  1=legacy, 2=modern */
#define VIRTIO_MMIO_DEVICE_ID        0x008   /* R  1=net, 2=blk, 3=console... */
#define VIRTIO_MMIO_VENDOR_ID        0x00C   /* R  */
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010   /* R  features[31:0] (sel) */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 /* W  */
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020   /* W  features[31:0] (sel) */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 /* W  */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE  0x028   /* W  legacy only */
#define VIRTIO_MMIO_QUEUE_SEL        0x030   /* W  */
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034   /* R  */
#define VIRTIO_MMIO_QUEUE_NUM        0x038   /* W  */
#define VIRTIO_MMIO_QUEUE_ALIGN      0x03C   /* W  legacy only */
#define VIRTIO_MMIO_QUEUE_PFN        0x040   /* RW legacy only */
#define VIRTIO_MMIO_QUEUE_READY      0x044   /* RW modern */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050   /* W  */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060   /* R  */
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064   /* W  */
#define VIRTIO_MMIO_STATUS           0x070   /* RW */
#define VIRTIO_MMIO_QUEUE_DESC_LOW   0x080   /* W  modern */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH  0x084   /* W  modern */
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090   /* W  modern */
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094  /* W  modern */
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0A0   /* W  modern */
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0A4  /* W  modern */
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0FC  /* R  modern */
#define VIRTIO_MMIO_CONFIG           0x100   /* RW device-specific config */

/* device IDs */
#define VIRTIO_ID_NET     1
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3

/* device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

/* 通用 feature bits */
#define VIRTIO_F_VERSION_1        32   /* modern 设备必须提供（bit32=高32位word bit0） */

/* ===== 后端框架 API ===== */

/* 初始化 virtio-mmio 后端框架（注册 block + net 设备）。
 * 在 linux_loader_init 之后、linux_launch 之前调用。 */
void virtio_mmio_init(void);

/* 处理一次 virtio-mmio 寄存器读/写（由 vmexit EPT violation 调用）。
 *   gpa   : 触发访问的 GPA（落在某设备 4KB 寄存器区）
 *   width : 访问宽度（字节），通常为 4
 *   value : 写操作的数据
 * 返回读操作的数据（写操作返回 0）。 */
u64  virtio_mmio_read(u64 gpa, int width);
void virtio_mmio_write(u64 gpa, u64 value, int width);

/* 后端驱动注册接口（virtio_blk.c / virtio_net.c 调用） */
struct virtio_backend {
    u32 device_id;          /* VIRTIO_ID_* */
    u64 gpa_base;           /* 该设备的 MMIO GPA 起始 */
    u32 irq;                /* guest ISA IRQ 号（cmdline virtio_mmio.device=:IRQ，
                             * legacy PIC 下 vector = 0x30 + irq；0 = 不注入中断） */
    u32 num_queues;         /* 队列数（blk=1, net=2） */
    u32 queue_size;         /* 每队列描述符数 */
    u64 device_features;    /* device 提供的 feature 位 */
    u32 config_len;         /* config 空间长度（从 0x100 起） */

    /* 读取 device config 空间（offset 相对 0x100）。返回 32 位值。 */
    u32 (*read_config)(u32 offset, int width);
    /* 通知：guest 写入 QUEUE_NOTIFY，后端处理该队列的请求。 */
    void (*queue_notify)(u32 queue_idx);
    /* 复位设备。 */
    void (*reset)(void);
};

/* 注册一个后端设备（内部使用，按 gpa_base 排序查找）。 */
int virtio_mmio_register(struct virtio_backend *dev);

/* 提供 guest GPA→host 虚拟地址转换（后端访问 virtqueue/描述符用）。
 * 通过 EPT 翻译 GPA 到 HPA，再加 HHDM offset 得到 host 虚拟地址。
 * 返回 NULL 表示 GPA 未映射。 */
void *virtio_gpa_to_host(u64 gpa);

/* ===== virtqueue 结构（Virtio Spec 1.1 §2.6 split ring） ===== */

/* 描述符：16 字节 */
struct virtq_desc {
    u64 addr;       /* GPA of data buffer */
    u32 len;        /* data length */
    u16 flags;      /* VIRTQ_DESC_F_* */
    u16 next;       /* next descriptor if F_NEXT */
} __attribute__((packed));

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2
#define VIRTQ_DESC_F_INDIRECT 4

/* avail ring（driver → device） */
struct virtq_avail {
    u16 flags;
    u16 idx;
    u16 ring[1];    /* queue_size entries */
} __attribute__((packed));

/* used ring 元素 */
struct virtq_used_elem {
    u32 id;         /* descriptor chain head index */
    u32 len;        /* bytes written (device→driver) */
} __attribute__((packed));

/* used ring（device → driver） */
struct virtq_used {
    u16 flags;
    u16 idx;
    struct virtq_used_elem ring[1];
} __attribute__((packed));

/* ===== 队列状态访问 API（后端 queue_notify 用） =====
 *
 * 后端在其 queue_notify(queue_idx) 回调中需要：
 *   1. 读 avail ring 取请求描述符链
 *   2. 处理请求（读/写 guest 数据 buffer）
 *   3. 写 used ring 回执
 * 以下函数按 device_id + queue_idx 定位队列，操作 guest 内存。 */

/* 取队列的 desc/avail/used 表 host 地址 + 队列大小。
 * 返回 0 成功（指针填充），负值失败（队列未 ready/未映射）。 */
int virtio_queue_get_ptrs(u32 device_id, u32 queue_idx,
                          struct virtq_desc **desc_out,
                          struct virtq_avail **avail_out,
                          struct virtq_used **used_out,
                          u32 *queue_num_out);

/* 更新 used ring idx（后端处理完一批请求后调用，让 guest 看到新回执）。
 * 由后端在写 used ring 元素后调用，内部做内存屏障。 */
void virtio_queue_bump_used(struct virtq_used *used, u16 new_idx);

#endif /* UTSM_VIRTIO_MMIO_H */
