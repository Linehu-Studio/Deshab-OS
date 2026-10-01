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
/* Shared-memory region (Virtio 1.1 §4.2.2). Linux virtio-gpu always probes
 * VIRTIO_GPU_SHM_ID_HOST_VISIBLE. A missing region MUST report length=-1;
 * length 0 is treated as a real window at address 0 and probe fails -EBUSY. */
#define VIRTIO_MMIO_SHM_SEL          0x0AC   /* W  */
#define VIRTIO_MMIO_SHM_LEN_LOW      0x0B0   /* R  */
#define VIRTIO_MMIO_SHM_LEN_HIGH     0x0B4   /* R  */
#define VIRTIO_MMIO_SHM_BASE_LOW     0x0B8   /* R  */
#define VIRTIO_MMIO_SHM_BASE_HIGH    0x0BC   /* R  */
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0FC  /* R  modern */
#define VIRTIO_MMIO_CONFIG           0x100   /* RW device-specific config */

/* device IDs */
#define VIRTIO_ID_NET     1
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_INPUT   18   /* VSCode Phase 2: virtio-input (keyboard/mouse) */
#define VIRTIO_ID_GPU     16   /* VSCode Phase 3: virtio-gpu 2D */

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
    /* VSCode Phase 2: 写入 device config 空间（offset 相对 0x100）。
     * virtio-input 用此通道设置 select/subsel 再读结果；其它设备 config
     * 只读，置 NULL 时框架忽略 config 写。 */
    void (*write_config)(u32 offset, u32 value);
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

/* VSCode Phase 2: 按 gpa_base 定位队列（用于 device_id 相同的多实例设备，
 * 如两个 virtio-input：键盘+鼠标同为 VIRTIO_ID_INPUT=18）。
 * 语义与 virtio_queue_get_ptrs 一致，只是查找键由 device_id 改为 gpa_base。 */
int virtio_queue_get_ptrs_by_gpa(u64 gpa_base, u32 queue_idx,
                                 struct virtq_desc **desc_out,
                                 struct virtq_avail **avail_out,
                                 struct virtq_used **used_out,
                                 u32 *queue_num_out);

/* 异步触发设备中断（设置 INTERRUPT_STATUS bit0 并向 guest 注入虚拟 IRQ）。
 * 用于后端在 QUEUE_NOTIFY 上下文之外完成工作（如周期轮询收到 RX 包）。
 * device_id: VIRTIO_ID_*；设备未注册或 irq==0 时为空操作。 */
void virtio_mmio_raise_irq(u32 device_id);

/* VSCode Phase 2: 按 gpa_base 触发中断。用于 device_id 相同的多实例设备
 *（两个 virtio-input 同为 18）：virtio_mmio_raise_irq(18) 只命中第一个。 */
void virtio_mmio_raise_irq_by_gpa(u64 gpa_base);

/* 周期轮询 virtio-net RX 路径（填充 guest 已投递的 RX buffer，
 * 有填充时自动注入 IRQ）。由 vmexit 的 preemption timer 路径调用。 */
void virtio_net_poll(void);

/* ===== VSCode Phase 2: virtio-input 后端 ===== */

/* Linux input event types/codes (subset, see <uapi/linux/input-event-codes.h>).
 * We only need the few event types we actually forward from Deshab. */
#define VIO_INPUT_EV_KEY  0x01
#define VIO_INPUT_EV_REL  0x02   /* relative movement (mouse) */
#define VIO_INPUT_EV_ABS  0x03
#define VIO_INPUT_EV_SYN  0x00   /* sync marker */

#define VIO_INPUT_REL_X   0x00
#define VIO_INPUT_REL_Y   0x01
#define VIO_INPUT_BTN_LEFT   0x110
#define VIO_INPUT_BTN_RIGHT  0x111
#define VIO_INPUT_BTN_MIDDLE 0x112

/* virtio_input_event (8 bytes, little-endian). Matches Linux struct
 * virtio_input_event in <uapi/linux/virtio_input.h>. */
struct virtio_input_event {
    u16 type;
    u16 code;
    u32 value;
} __attribute__((packed));

/* 注册键盘+鼠标两个 virtio-input 后端（无设备时安全空操作）。
 * 在 virtio_mmio_init 中调用。 */
void virtio_input_backend_init(void);

/* 推送键盘事件（type=EV_KEY, code=linux keycode, value=0/1/2）。
 * value: 0=release, 1=press, 2=repeat。 */
void virtio_input_push_keyboard(u16 code, u32 value);

/* 推送鼠标事件。三种便捷封装：
 *   push_mouse_rel(axis, value)  — 相对位移 (EV_REL, REL_X/REL_Y)
 *   push_mouse_button(btn, value)— 按键 (EV_KEY, BTN_LEFT/RIGHT/MIDDLE)
 *   push_mouse_syn()             — 同步标记 (EV_SYN) */
void virtio_input_push_mouse_rel(u16 axis, i32 value);
void virtio_input_push_mouse_button(u16 btn, u32 value);
void virtio_input_push_mouse_syn(void);

/* 周期轮询：把挂起的输入事件排空到 guest eventq 已投递的空 buffer 中，
 * 有事件投递时自动注入 IRQ。由 vmexit 的 preemption timer 路径调用。 */
void virtio_input_poll(void);

/* ===== VSCode Phase 3: virtio-gpu 2D 后端 ===== */

/* Scanout 查询结果：desktop 在 IDE attached 时读取，把 guest 图形 blit 到
 * Deshab framebuffer 的 IDE host 区域。
 *   host_vaddr : scanout 影子缓冲的 host 虚拟地址（位于 graphics surface pool，
 *                16MB，足够 1920×1080×4）。guest virtio-gpu 驱动经
 *                TRANSFER_TO_HOST_2D + RESOURCE_FLUSH 把帧合成到这里。
 *   width/height/stride : scanout 几何（XRGB8888，stride = width*4）
 *   dirty   : 1 = 自上次查询以来有新帧合成（查询会清零该位）
 *   enabled : 1 = scanout 已绑定一个 2D resource（X server 已 SET_SCANOUT）
 *   dirty_x/y/w/h : Phase 7 累积 dirty rect（scanout 坐标系并集），
 *                   查询随 dirty 一并清零；dirty=0 时全为 0。
 *                   desktop 据此只 blit 变化区域（尾部追加，ABI 兼容）。 */
struct virtio_gpu_scanout_info {
    void *host_vaddr;
    u32 width;
    u32 height;
    u32 stride;
    u32 dirty;
    u32 enabled;
    u32 dirty_x;
    u32 dirty_y;
    u32 dirty_w;
    u32 dirty_h;
    /* P7.6: hardware cursor state（尾部追加，ABI 兼容）。
     * cursor_visible=1 时 cursor_bitmap 指向 ARGB 像素（row stride=64），
     * desktop 在 blit 后 alpha-blend 叠加到 IDE host 区域。 */
    u32 cursor_visible;
    u32 cursor_x;
    u32 cursor_y;
    u32 cursor_hot_x;
    u32 cursor_hot_y;
    u32 cursor_w;
    u32 cursor_h;
    void *cursor_bitmap;
};

/* 注册 virtio-gpu 2D 后端（slot 5 @ 0xF4005000 IRQ10）。
 * 在 virtio_mmio_init 中调用。无 surface pool 时安全降级（scanout disabled）。 */
void virtio_gpu_backend_init(void);

/* 查询当前 scanout 状态（desktop blit 用）。返回 0 成功，-1 后端未就绪。 */
int virtio_gpu_get_scanout_info(struct virtio_gpu_scanout_info *out);

#endif /* UTSM_VIRTIO_MMIO_H */
