#include <utsm/types.h>

void outb(u16 port, u8 value);
u8 inb(u16 port);

#define COM1 0x3F8

/* ---- TSC-based timing (实机要求: 用 CPU 频率计算, 不用循环) ---- */
static u64 g_tsc_per_ms = 0;

static inline u64 rdtsc_ser(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* 用 PIT (8254, 1.193182 MHz) 校准 TSC 频率 */
static void tsc_calibrate_ser(void) {
    outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = rdtsc_ser();
    u16 prev = 0; u64 loops = 0;
    for (;;) {
        outb(0x43, 0x00);
        u16 cur = (u16)inb(0x40) | ((u16)inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur; loops++;
    }
    u64 tsc_end = rdtsc_ser();
    g_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

static int serial_ready(void) {
    return (inb(COM1 + 5) & 0x20) != 0;
}

/* 实机: 基于 TSC 的 100ms 超时等待 */
static int serial_wait_ready(void) {
    if (!g_tsc_per_ms) {
        /* 兜底: 校准未完成时使用循环 */
        for (u64 spin = 0; spin < 1000000ULL; spin++) {
            if (serial_ready()) return 1;
        }
        return 0;
    }
    u64 deadline = rdtsc_ser() + g_tsc_per_ms * 100;  /* 100ms 超时 */
    while (rdtsc_ser() < deadline) {
        if (serial_ready()) return 1;
        __asm__ volatile("pause");
    }
    return 0;
}

void serial_init(void) {
    /* 实机要求: 先校准 TSC, 再使用基于 CPU 频率的延迟 */
    tsc_calibrate_ser();
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');
    }
    if (!serial_wait_ready()) {
        return;
    }
    outb(COM1, (u8)c);
}

void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

/* 非阻塞读取 host 串口一个字节（QEMU stdio → guest COM1 输入桥）。
 * 返回 0-255，无数据返回 -1。供 vmexit 在 guest resume 前轮询。 */
int serial_try_read(void) {
    if (inb(COM1 + 5) & 0x01) {
        return inb(COM1);
    }
    return -1;
}

/* 导出 PIT 校准的 TSC 频率（kHz 量级，单位 ticks/ms），
 * 供 VMX preemption timer 与 guest PIT tick 注入使用。 */
u64 serial_tsc_per_ms(void) {
    return g_tsc_per_ms;
}
