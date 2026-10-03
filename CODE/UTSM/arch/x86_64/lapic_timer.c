/* lapic_timer.c — 宿主自持 LAPIC 定时器（调度器 tick 源）
 *
 * 为什么不用 PIT 周期中断：默认 FUCK apic_route=1 时 apic.drv 验证完
 * tick 链路后重新 mask GSI2（PIT IRQ0 线），PIT 周期中断不可达；
 * LAPIC timer 不经 IOAPIC/PIC 路由，与路由模式解耦，且是 ROADMAP
 * Phase 2 "LAPIC timer 替代 PIT" 的目标方案。
 *
 * 流程：MSR 0x1B → 物理基址 → map_mmio（SAS-R0 直接扩当前 CR3）
 *       → PIT ch0 单发 10ms 采样 CCR 差值校准
 *       → irq_vector_alloc 动态向量 + LVT periodic（先 mask，start 再放开）
 */

#include <utsm/lapic_timer.h>
#include <utsm/pit.h>
#include <utsm/paging.h>
#include <utsm/idt.h>
#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/panic.h>

extern void outb(u16 port, u8 value);
extern u8 inb(u16 port);

#define MSR_IA32_APIC_BASE 0x1Bu
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2APIC   (1ULL << 10)
#define LAPIC_DEFAULT_PHYS 0xFEE00000ULL

#define LAPIC_EOI        0x0B0u
#define LAPIC_TPR        0x080u
#define LAPIC_SPURIOUS   0x0F0u
#define LAPIC_LVT_TIMER  0x320u
#define LAPIC_DIV_CONF   0x3E0u
#define LAPIC_INIT_CNT   0x380u
#define LAPIC_CUR_CNT    0x390u

#define LVT_MASK   (1u << 16)
#define LVT_PERIODIC (1u << 17)

static volatile u32 *g_lapic = 0;
static int g_timer_vector = -1;

static inline u64 rdmsr_(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline void wrmsr_(u32 msr, u64 v) {
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static u32 lapic_read(u32 reg)  { return g_lapic[reg / 4]; }
static void lapic_write(u32 reg, u32 v) { g_lapic[reg / 4] = v; }

int lapic_timer_init(u32 freq_hz) {
    if (freq_hz == 0) freq_hz = 100;

    if (!g_lapic) {
        u64 base = rdmsr_(MSR_IA32_APIC_BASE);
        if (!(base & APIC_BASE_ENABLE)) {
            wrmsr_(MSR_IA32_APIC_BASE, base | APIC_BASE_ENABLE);
            base = rdmsr_(MSR_IA32_APIC_BASE);
        }
        if (base & APIC_BASE_X2APIC) {
            /* 【插桩白名单】设计内降级：x2APIC 不支持 MMIO timer，降级协作模式 */
            log_warn("[LAPIC] x2APIC mode, MMIO timer unsupported");
            return -1;
        }
        u64 phys = base & 0xFFFFF000ULL;
        if (phys == 0) phys = LAPIC_DEFAULT_PHYS;

        const dkm_mmio_api *mm = paging_get_api();
        if (!mm || !mm->map_mmio) return -2;
        if (mm->map_mmio(phys, 0x1000) != 0) {
            log_warn("[LAPIC] map_mmio failed");
            panic_full("LAPIC-E01 MAP_MMIO FAILED",
                       "lapic_timer_init: LAPIC MMIO mapping failed", 0);
            return -3;
        }
        u64 hhdm = 0;
        const dkm_kernel_api *api = dkm_get_kernel_api();
        if (api) hhdm = api->hhdm_offset;
        g_lapic = (volatile u32 *)(hhdm + phys);
        if (mm->is_mapped && !mm->is_mapped((u64)g_lapic)) {
            g_lapic = 0;
            log_warn("[LAPIC] mapped page not reachable");
            panic_full("LAPIC-E02 MAPPED PAGE NOT REACHABLE",
                       "lapic_timer_init: mapped LAPIC page not reachable", 0);
            return -4;
        }

        /* 软件使能（spurious bit8）；vector 保留既有值，无则 0xFF */
        u32 svr = lapic_read(LAPIC_SPURIOUS);
        if (!(svr & 0x100u)) {
            u8 vec = (u8)(svr & 0xFFu);
            if (vec == 0) vec = 0xFFu;
            lapic_write(LAPIC_SPURIOUS, (u32)vec | 0x100u);
        }
        lapic_write(LAPIC_TPR, 0);
        log_info("[LAPIC] mmio mapped");
        log_hex64("[LAPIC]   phys=", phys);
    }

    /* ---- PIT 单发校准：divide=16，10ms 窗口采样 CCR 差值 ---- */
    lapic_write(LAPIC_DIV_CONF, 0x3);              /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, (u32)0xFF | LVT_MASK | LVT_PERIODIC); /* 校准期 mask */
    lapic_write(LAPIC_INIT_CNT, 0xFFFFFFFFu);
    u32 ccr0 = 0;
    for (int i = 0; i < 1000; i++) {
        ccr0 = lapic_read(LAPIC_CUR_CNT);
        if (ccr0 != 0) break;
        __asm__ volatile("pause");
    }

    pit_oneshot_ms(10);
    u16 prev = pit_latch_count();
    u32 loops = 0;
    for (;;) {
        u16 cur = pit_latch_count();
        if (cur > prev && loops > 10) break;       /* 回绕 = 10ms 到期 */
        prev = cur;
        loops++;
    }
    u32 ccr1 = lapic_read(LAPIC_CUR_CNT);
    u64 delta = (u64)ccr0 - (u64)ccr1;             /* 16 分频时钟 10ms tick 数 */
    if (delta < 1000) {
        log_warn("[LAPIC] timer calibration implausible");
        panic_full("LAPIC-E03 CALIBRATION IMPLAUSIBLE",
                   "lapic_timer_init: PIT-vs-LAPIC calibration implausible", 0);
        lapic_timer_stop();
        return -5;
    }

    /* 期望周期 = delta * (1000 / freq_hz) / 10 */
    u64 init = delta * 1000ULL / (u64)freq_hz / 10ULL;
    if (init == 0) init = 1;
    if (init > 0xFFFFFFFFULL) init = 0xFFFFFFFFULL;

    if (g_timer_vector < 0) {
        int vec = irq_vector_alloc();
        if (vec < 0) {
            log_warn("[LAPIC] irq_vector_alloc failed");
            panic_full("LAPIC-E04 IRQ VECTOR ALLOC FAILED",
                       "lapic_timer_init: dynamic vector allocation failed", 0);
            return -6;
        }
        g_timer_vector = vec;
    }

    /* 按校准值定频编程（LVT 保持 mask，start() 再放开）。
     * 顺序：divide config → LVT(masked) → initial count。 */
    lapic_write(LAPIC_DIV_CONF, 0x3);
    lapic_write(LAPIC_LVT_TIMER,
                ((u32)g_timer_vector & 0xFFu) | LVT_MASK | LVT_PERIODIC);
    lapic_write(LAPIC_INIT_CNT, (u32)init);

    log_info("[LAPIC] timer calibrated");
    log_hex64("[LAPIC]   ticks/10ms=", delta);
    log_hex64("[LAPIC]   init_count=", init);
    log_hex64("[LAPIC]   vector=", (u64)g_timer_vector);
    return 0;
}

int lapic_timer_vector(void) {
    return g_timer_vector;
}

void lapic_timer_start(void) {
    if (!g_lapic || g_timer_vector < 0) return;
    /* 保持 divide/init 不变，仅解除 mask（periodic 模式已配） */
    lapic_write(LAPIC_LVT_TIMER,
                ((u32)g_timer_vector & 0xFFu) | LVT_PERIODIC);
}

void lapic_timer_stop(void) {
    if (g_lapic) {
        lapic_write(LAPIC_LVT_TIMER, lapic_read(LAPIC_LVT_TIMER) | LVT_MASK);
        lapic_write(LAPIC_INIT_CNT, 0);
    }
    if (g_timer_vector >= 0) {
        irq_vector_free(g_timer_vector);
        g_timer_vector = -1;
    }
}

void lapic_timer_eoi(void) {
    if (g_lapic) {
        lapic_write(LAPIC_EOI, 0);
    }
}
