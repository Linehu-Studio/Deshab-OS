/* virtio_input.c — UTSM virtio-input backend (keyboard + mouse).
 *
 * VSCode integration Phase 2: forward Deshab desktop input events to the
 * Linux guest via two virtio-input devices (device_id=18):
 *   slot 3 @ 0xF4003000 IRQ8 — keyboard (evdev /dev/input/event0)
 *   slot 4 @ 0xF4004000 IRQ9 — mouse    (evdev /dev/input/event1)
 *
 * Data flow:
 *   1. Deshab desktop (DSK) calls virtio_input_push_keyboard/push_mouse_* on
 *      a PS/2 scancode / mouse packet. The event lands in a pending ring.
 *   2. vmexit_before_resume() calls virtio_input_poll() every ~1ms.
 *   3. poll drains pending events into guest eventq buffers the driver posted
 *      (device→driver direction), bumps the used ring, and raises IRQ8/IRQ9.
 *   4. Linux evdev delivers the events to userspace (Xorg/libinput).
 *
 * The statusq (queue 1, driver→device) is consumed but ignored — we only
 * acknowledge status events (e.g. keyboard LED state) with len=0.
 *
 * Config space implements the select/subsel state machine per virtio-input
 * spec: driver writes select+subsel, then reads the 128-byte payload.
 */

#include <utsm/virtio_mmio.h>
#include <utsm/linux_loader.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* ===== Config space layout (virtio-input spec §5) =====
 *   [0]     select   — which config item
 *   [1]     subsel   — sub-selector (e.g. event type for EV_BITS)
 *   [2]     size     — valid payload bytes (0..128)
 *   [3..7]  reserved
 *   [8..135] payload (string or bitmap) */
#define VIO_CFG_LEN     136
#define VIO_CFG_PAYLOAD 128

/* select values (virtio_input_config select field) */
#define VIO_CFG_ID_NAME    1
#define VIO_CFG_ID_SERIAL  2
#define VIO_CFG_ID_DEVIDS  3
#define VIO_CFG_PROP_BITS  4
#define VIO_CFG_EV_BITS    5

/* Pending event ring depth. 256 events is generous headroom over the
 * ~1kHz poll rate — if the guest hasn't posted eventq buffers for 256ms
 * of input, events start dropping (oldest first). */
#define VIO_PEND_MAX 256

struct virtio_input_dev {
    u64 gpa_base;
    u32 irq;
    const char *name;

    /* Config select/subsel state (written by guest, read back as payload) */
    u8 cfg_select;
    u8 cfg_subsel;
    u8 cfg_buf[VIO_CFG_LEN];

    /* Pending event ring (SPSC): push_* produces, poll consumes.
     * No lock — UTSM is cooperative: push runs in DSK context (guest
     * parked), poll runs in vmexit_before_resume (just before resume).
     * They never execute concurrently. */
    struct virtio_input_event pending[VIO_PEND_MAX];
    u32 pend_head;
    u32 pend_tail;
    u32 pend_count;

    /* Per-queue avail cursor (last processed avail->idx). */
    u16 last_avail_event;
    u16 last_avail_status;

    struct virtio_backend backend;
};

static struct virtio_input_dev g_kbd;
static struct virtio_input_dev g_mouse;

/* ===== Config space rebuild =====
 * Called whenever the guest writes select/subsel. Fills cfg_buf with the
 * full 136-byte config image (select, subsel, size, reserved, payload). */
static void input_rebuild_cfg(struct virtio_input_dev *dev) {
    u8 *buf = dev->cfg_buf;
    buf[0] = dev->cfg_select;
    buf[1] = dev->cfg_subsel;
    buf[2] = 0;  /* size, set below */
    for (int i = 3; i < 8; i++) buf[i] = 0;

    u8 *payload = buf + 8;
    for (int i = 0; i < VIO_CFG_PAYLOAD; i++) payload[i] = 0;
    u8 size = 0;

    switch (dev->cfg_select) {
    case VIO_CFG_ID_NAME: {
        const char *s = dev->name;
        u32 i = 0;
        while (s[i] && i < VIO_CFG_PAYLOAD) { payload[i] = (u8)s[i]; i++; }
        size = (u8)i;
        break;
    }
    case VIO_CFG_ID_SERIAL:
        size = 0;  /* no serial */
        break;
    case VIO_CFG_ID_DEVIDS: {
        /* Bitmap of supported event types (bit N = EV_TYPE N).
         * keyboard: EV_KEY only; mouse: EV_KEY + EV_REL. */
        payload[VIO_INPUT_EV_KEY / 8] |= (u8)(1u << (VIO_INPUT_EV_KEY % 8));
        if (dev == &g_mouse) {
            payload[VIO_INPUT_EV_REL / 8] |= (u8)(1u << (VIO_INPUT_EV_REL % 8));
        }
        u32 max_ev = (dev == &g_mouse) ? VIO_INPUT_EV_REL : VIO_INPUT_EV_KEY;
        size = (u8)(max_ev / 8 + 1);
        break;
    }
    case VIO_CFG_PROP_BITS:
        size = 0;
        break;
    case VIO_CFG_EV_BITS: {
        /* subsel = event type; payload = bitmap of supported event codes. */
        u16 ev = dev->cfg_subsel;
        if (dev == &g_kbd && ev == VIO_INPUT_EV_KEY) {
            /* Keyboard: advertise keys 1..248 (ESCAPE..KEY_WWW). The driver
             * enumerates all set bits; only forwarded keys will ever arrive. */
            for (u32 i = 1; i <= 248; i++) {
                payload[i / 8] |= (u8)(1u << (i % 8));
            }
            size = 32;  /* 256 bits = 32 bytes */
        } else if (dev == &g_mouse) {
            if (ev == VIO_INPUT_EV_KEY) {
                payload[VIO_INPUT_BTN_LEFT   / 8] |= (u8)(1u << (VIO_INPUT_BTN_LEFT   % 8));
                payload[VIO_INPUT_BTN_RIGHT  / 8] |= (u8)(1u << (VIO_INPUT_BTN_RIGHT  % 8));
                payload[VIO_INPUT_BTN_MIDDLE / 8] |= (u8)(1u << (VIO_INPUT_BTN_MIDDLE % 8));
                size = (u8)(VIO_INPUT_BTN_MIDDLE / 8 + 1);
            } else if (ev == VIO_INPUT_EV_REL) {
                payload[VIO_INPUT_REL_X / 8] |= (u8)(1u << (VIO_INPUT_REL_X % 8));
                payload[VIO_INPUT_REL_Y / 8] |= (u8)(1u << (VIO_INPUT_REL_Y % 8));
                size = 1;
            }
        }
        break;
    }
    default:
        size = 0;
        break;
    }

    buf[2] = size;
}

/* Read width bytes from the config buffer at offset (little-endian u32). */
static u32 input_read_config_at(struct virtio_input_dev *dev, u32 offset, int width) {
    if (offset >= VIO_CFG_LEN) return 0;
    u32 val = 0;
    for (int i = 0; i < width && offset + i < VIO_CFG_LEN; i++) {
        val |= (u32)dev->cfg_buf[offset + i] << (8 * i);
    }
    return val;
}

/* ===== Per-device thin wrappers (backend callbacks have no ctx) ===== */

static u32 kbd_read_config(u32 offset, int width)   { return input_read_config_at(&g_kbd, offset, width); }
static u32 mouse_read_config(u32 offset, int width) { return input_read_config_at(&g_mouse, offset, width); }

static void kbd_write_config(u32 offset, u32 value) {
    if (offset == 0) { g_kbd.cfg_select = (u8)value; input_rebuild_cfg(&g_kbd); }
    else if (offset == 1) { g_kbd.cfg_subsel = (u8)value; input_rebuild_cfg(&g_kbd); }
}
static void mouse_write_config(u32 offset, u32 value) {
    if (offset == 0) { g_mouse.cfg_select = (u8)value; input_rebuild_cfg(&g_mouse); }
    else if (offset == 1) { g_mouse.cfg_subsel = (u8)value; input_rebuild_cfg(&g_mouse); }
}

static void input_reset_dev(struct virtio_input_dev *dev) {
    dev->cfg_select = 0;
    dev->cfg_subsel = 0;
    dev->pend_head = 0;
    dev->pend_tail = 0;
    dev->pend_count = 0;
    dev->last_avail_event = 0;
    dev->last_avail_status = 0;
    input_rebuild_cfg(dev);
}
static void kbd_reset(void)   { input_reset_dev(&g_kbd); }
static void mouse_reset(void) { input_reset_dev(&g_mouse); }

/* ===== Eventq drain (device → driver) =====
 * Walk the eventq avail ring: for each empty buffer the driver posted, pop
 * one pending event, write it into the buffer, complete the used entry.
 * Returns the number of buffers filled. */
static int input_drain_eventq(struct virtio_input_dev *dev) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs_by_gpa(dev->gpa_base, 0, &desc, &avail, &used, &qnum) != 0)
        return 0;

    int filled = 0;
    u16 cur = dev->last_avail_event;
    u16 used_idx = used->idx;
    int budget = 32;  /* cap per poll to bound vmexit latency */

    while (budget-- > 0 && dev->pend_count > 0) {
        if (cur == avail->idx) break;  /* no empty buffers posted */

        u16 head = avail->ring[cur % qnum];
        if (head >= qnum) { cur++; continue; }  /* bad index, skip */

        struct virtq_desc *d = &desc[head];
        struct virtio_input_event *buf = (struct virtio_input_event *)virtio_gpa_to_host(d->addr);
        if (!buf || d->len < (u32)sizeof(struct virtio_input_event)) {
            /* buffer too small or unmapped: return it empty so guest recycles */
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = 0;
            used_idx++;
            cur++;
            filled++;
            continue;
        }

        /* Pop one pending event into the guest buffer */
        u32 idx = dev->pend_tail;
        buf->type  = dev->pending[idx].type;
        buf->code  = dev->pending[idx].code;
        buf->value = dev->pending[idx].value;
        dev->pend_tail = (idx + 1) % VIO_PEND_MAX;
        dev->pend_count--;

        used->ring[used_idx % qnum].id = head;
        used->ring[used_idx % qnum].len = sizeof(struct virtio_input_event);
        used_idx++;
        cur++;
        filled++;
    }

    if (filled > 0) {
        virtio_queue_bump_used(used, used_idx);
        dev->last_avail_event = cur;
    }
    return filled;
}

/* ===== Statusq drain (driver → device) =====
 * The driver occasionally posts status events (e.g. keyboard LED state).
 * We don't act on them — just complete the buffers with len=0 so the driver
 * can recycle them and the avail ring doesn't fill up. */
static int input_drain_statusq(struct virtio_input_dev *dev) {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    u32 qnum;
    if (virtio_queue_get_ptrs_by_gpa(dev->gpa_base, 1, &desc, &avail, &used, &qnum) != 0)
        return 0;

    int filled = 0;
    u16 cur = dev->last_avail_status;
    u16 used_idx = used->idx;
    int budget = 8;

    while (budget-- > 0 && cur != avail->idx) {
        u16 head = avail->ring[cur % qnum];
        if (head < qnum) {
            used->ring[used_idx % qnum].id = head;
            used->ring[used_idx % qnum].len = 0;
            used_idx++;
            filled++;
        }
        cur++;
    }

    if (filled > 0) {
        virtio_queue_bump_used(used, used_idx);
        dev->last_avail_status = cur;
    }
    return filled;
}

/* ===== Backend queue_notify callbacks =====
 * eventq notify (queue 0): the driver just posted empty buffers — try to
 *   drain pending events now. IRQ is raised by the framework after notify.
 * statusq notify (queue 1): the driver sent a status event — consume it. */
static void kbd_queue_notify(u32 queue_idx) {
    if (queue_idx == 0) {
        if (input_drain_eventq(&g_kbd) > 0)
            virtio_mmio_raise_irq_by_gpa(g_kbd.gpa_base);
    } else if (queue_idx == 1) {
        input_drain_statusq(&g_kbd);
    }
}
static void mouse_queue_notify(u32 queue_idx) {
    if (queue_idx == 0) {
        if (input_drain_eventq(&g_mouse) > 0)
            virtio_mmio_raise_irq_by_gpa(g_mouse.gpa_base);
    } else if (queue_idx == 1) {
        input_drain_statusq(&g_mouse);
    }
}

/* ===== Pending event push API (called by DSK / desktop input forwarding) ===== */

static void input_push(struct virtio_input_dev *dev, u16 type, u16 code, u32 value) {
    if (dev->pend_count >= VIO_PEND_MAX) {
        /* Ring full: drop the oldest event to make room for the newest.
         * Dropping old (not new) preserves the most recent input state,
         * which is what matters for interactive feel. */
        dev->pend_tail = (dev->pend_tail + 1) % VIO_PEND_MAX;
        dev->pend_count--;
    }
    u32 idx = dev->pend_head;
    dev->pending[idx].type = type;
    dev->pending[idx].code = code;
    dev->pending[idx].value = value;
    dev->pend_head = (idx + 1) % VIO_PEND_MAX;
    dev->pend_count++;
}

void virtio_input_push_keyboard(u16 code, u32 value) {
    /* code = linux keycode (KEY_A..), value = 0=release, 1=press, 2=repeat */
    input_push(&g_kbd, VIO_INPUT_EV_KEY, code, value);
}

void virtio_input_push_mouse_rel(u16 axis, i32 value) {
    /* axis = VIO_INPUT_REL_X or VIO_INPUT_REL_Y */
    input_push(&g_mouse, VIO_INPUT_EV_REL, axis, (u32)value);
}

void virtio_input_push_mouse_button(u16 btn, u32 value) {
    /* btn = VIO_INPUT_BTN_LEFT/RIGHT/MIDDLE, value = 0=release, 1=press */
    input_push(&g_mouse, VIO_INPUT_EV_KEY, btn, value);
}

void virtio_input_push_mouse_syn(void) {
    /* SYN_REPORT marks the end of a logical input frame (e.g. a complete
     * mouse movement + button change). libinput relies on this to deliver
     * a single consolidated event to clients. */
    input_push(&g_mouse, VIO_INPUT_EV_SYN, 0, 0);
}

/* ===== Periodic poll (called from vmexit_before_resume) ===== */
void virtio_input_poll(void) {
    int kbd_filled = input_drain_eventq(&g_kbd);
    int mouse_filled = input_drain_eventq(&g_mouse);
    if (kbd_filled > 0)   virtio_mmio_raise_irq_by_gpa(g_kbd.gpa_base);
    if (mouse_filled > 0) virtio_mmio_raise_irq_by_gpa(g_mouse.gpa_base);
}

/* ===== Backend init ===== */

static struct virtio_backend g_kbd_backend = {
    .device_id      = VIRTIO_ID_INPUT,
    .gpa_base       = VIRTIO_MMIO_KBD_GPA,
    .irq            = 8,                     /* cmdline :8, vector 0x38 */
    .num_queues     = 2,                      /* eventq + statusq */
    .queue_size     = 64,
    .device_features = (1ULL << VIRTIO_F_VERSION_1),
    .config_len     = VIO_CFG_LEN,
    .read_config    = kbd_read_config,
    .write_config   = kbd_write_config,
    .queue_notify   = kbd_queue_notify,
    .reset          = kbd_reset,
};

static struct virtio_backend g_mouse_backend = {
    .device_id      = VIRTIO_ID_INPUT,
    .gpa_base       = VIRTIO_MMIO_MOUSE_GPA,
    .irq            = 9,                     /* cmdline :9, vector 0x39 */
    .num_queues     = 2,
    .queue_size     = 64,
    .device_features = (1ULL << VIRTIO_F_VERSION_1),
    .config_len     = VIO_CFG_LEN,
    .read_config    = mouse_read_config,
    .write_config   = mouse_write_config,
    .queue_notify   = mouse_queue_notify,
    .reset          = mouse_reset,
};

void virtio_input_backend_init(void) {
    g_kbd.gpa_base = VIRTIO_MMIO_KBD_GPA;
    g_kbd.irq = 8;
    g_kbd.name = "Deshab Virtual Keyboard";
    input_reset_dev(&g_kbd);

    g_mouse.gpa_base = VIRTIO_MMIO_MOUSE_GPA;
    g_mouse.irq = 9;
    g_mouse.name = "Deshab Virtual Mouse";
    input_reset_dev(&g_mouse);

    virtio_mmio_register(&g_kbd_backend);
    virtio_mmio_register(&g_mouse_backend);

    log_info("[VINPUT] keyboard + mouse backends registered");
}
