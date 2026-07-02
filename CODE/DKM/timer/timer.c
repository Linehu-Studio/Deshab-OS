/* DKM Timer Driver - PIT (Programmable Interval Timer)
 * Stage 0 platform driver, provides "timer".
 * No IDT/IRQ required - uses PIT counter polling for busy-wait delay.
 */

#include <stdint.h>

/* ---------------------------------------------------------------
 * DKM ABI types (standalone, no kernel headers)
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

/* ---------------------------------------------------------------
 * PIT I/O ports
 * --------------------------------------------------------------- */
#define PIT_CH0_DATA  0x40
#define PIT_CH1_DATA  0x41
#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43

/* PIT frequency: 1.193182 MHz */
#define PIT_BASE_FREQ 1193182ULL

static const char *const g_provides[] = { "timer" };

__attribute__((visibility("default"), used))
const struct dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,
    .abi_version    = DKM_ABI_VERSION,
    .desc_size      = sizeof(struct dkm_driver_desc),
    .name           = "timer",
    .version        = "0.1.0",
    .vendor         = "Deshab",
    .driver_class   = 4,   /* DKM_CLASS_TIMER */
    .stage          = 0,
    .flags          = 1,   /* DKM_F_REQUIRED */
    .priority       = 0,
    .depends        = NULL,
    .depends_count  = 0,
    .provides       = g_provides,
    .provides_count = 1,
    .min_kernel_abi = 1,
    .feature_bits   = 0,
    .reserved0      = 0,
    .reserved1      = 0
};

/* ---------------------------------------------------------------
 * Inline assembly for port I/O (works in Ring0 SAS-R0)
 * --------------------------------------------------------------- */
static __inline__ void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static __inline__ u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ---------------------------------------------------------------
 * PIT operations
 * --------------------------------------------------------------- */
static const struct dkm_log_api *g_log;

static void pit_set_channel(u8 channel, u8 mode, u16 count) {
    u8 cmd = (u8)((channel << 6) | (3 << 4) | (mode << 1));
    outb(PIT_CMD, cmd);
    outb((u16)(PIT_CH0_DATA + channel), (u8)(count & 0xff));
    outb((u16)(PIT_CH0_DATA + channel), (u8)((count >> 8) & 0xff));
}

/* Read back current counter value via latch command */
static u16 pit_read_counter(void) {
    u8 cmd = 0xD0;  /* read-back: channel 0, latch count, no status */
    outb(PIT_CMD, cmd);
    u8 lo = inb(PIT_CH0_DATA);
    u8 hi = inb(PIT_CH0_DATA);
    return (u16)(((u16)hi << 8) | lo);
}

/* Busy-wait delay using PIT channel 0 counter.
 * count: PIT divisor (1193182 = ~1 second)
 * The counter counts DOWN from 'count' to 0.
 * We set it to 'count', then poll until it reaches near 0.
 */
static void pit_delay(u16 count) {
    pit_set_channel(0, 0, count);       /* mode 0: interrupt on terminal count */
    u16 prev = pit_read_counter();
    u64 loops = 0;
    while (1) {
        u16 cur = pit_read_counter();
        if (cur > prev && loops > 10) {
            break;  /* counter wrapped → terminal count reached */
        }
        prev = cur;
        loops++;
    }
}

/* ---------------------------------------------------------------
 * Driver entry points
 * --------------------------------------------------------------- */
__attribute__((visibility("default")))
int driver_init(const struct dkm_kernel_api *api,
                struct dkm_driver_handle *handle) {
    (void)handle;

    if (!api || !api->log) {
        return -1;
    }
    g_log = api->log;

    g_log->info("[timer] PIT driver initializing");

    /* Quick calibration: set PIT to max count, measure how many
     * iterations of read_counter() happen before terminal count.
     */
    pit_set_channel(0, 0, 0xFFFF);      /* 0xFFFF = max, ~55ms at 1.193 MHz */
    u16 prev = pit_read_counter();
    u64 cal_loops = 0;
    while (1) {
        u16 cur = pit_read_counter();
        cal_loops++;
        if (cur > prev && cal_loops > 5) break;
        prev = cur;
    }

    /* Expected: PIT_BASE_FREQ / 0xFFFF ≈ 18.2 Hz, period ≈ 55 ms */
    g_log->info("[timer] PIT calibration complete");
    g_log->info("[timer] PIT channel 0 operational");

    /* Do a quick ~100ms delay demo */
    g_log->info("[timer] running 100ms busy-wait demo");
    pit_delay((u16)(PIT_BASE_FREQ / 10));   /* ~0.1 sec = 119318 counts */
    g_log->info("[timer] 100ms delay done");

    g_log->info("[timer] driver ready");
    return 0;
}

__attribute__((visibility("default")))
int driver_exit(struct dkm_driver_handle *handle) {
    (void)handle;
    return 0;
}
