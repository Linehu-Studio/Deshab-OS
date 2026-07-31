/* instr.c — Deshab 插桩系统初始化与全局配置管理
 *
 * 从 FUCK [debug] 配置段加载插桩参数，初始化全局配置变量。
 * 在 kernel_main() 中调用 instr_init(&g_boot_cfg) 启用。
 */
#include <utsm/instr.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "ini_parser.h"

/* ---- 全局配置变量（供 instr.h 宏条件判断） ---- */
u32 g_instr_enabled       = 0;
u32 g_instr_log_level     = INSTR_LOG_INFO;
u32 g_instr_probe_enable  = 0;
u32 g_instr_stat_interval = 256;
u32 g_instr_module_flags  = 0;
u64 instr_tsc_per_ms     = 0;

/* ---- 低级 I/O（TSC 校准用，与 arch/x86_64/io.c 独立避免链接冲突） ---- */
static void instr_outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}
static u8 instr_inb(u16 port) {
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* TSC 读取 */
u64 instr_rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* TSC 校准：PIT ch0 ~10ms 区间，与 serial.c / dkm_shared.h 相同逻辑 */
static void instr_tsc_calibrate(void) {
    instr_outb(0x43, 0x30);       /* ch0, lo+hi, mode 0, binary */
    instr_outb(0x40, 0x7c);       /* 11932 low = ~10ms */
    instr_outb(0x40, 0x2e);       /* 11932 high */
    u64 tsc_start = instr_rdtsc();
    u16 prev = 0;
    u64 loops = 0;
    for (;;) {
        instr_outb(0x43, 0x00);
        u16 cur = (u16)instr_inb(0x40) | ((u16)instr_inb(0x40) << 8);
        if (cur > prev && loops > 10) break;
        prev = cur;
        loops++;
    }
    u64 tsc_end = instr_rdtsc();
    instr_tsc_per_ms = (tsc_end - tsc_start) / 10;
}

void instr_init(const void *cfg_ptr) {
    const ini_config *cfg = (const ini_config *)cfg_ptr;

    /* 从 FUCK [debug] 段读取配置 */
    g_instr_enabled       = (u32)ini_get_bool(cfg, "debug", "enabled", 0);
    g_instr_log_level     = (u32)ini_get_int(cfg, "debug", "log_level", INSTR_LOG_INFO);
    g_instr_probe_enable  = (u32)ini_get_bool(cfg, "debug", "probe_enable", 0);
    g_instr_stat_interval = (u32)ini_get_int(cfg, "debug", "stat_summary_interval", 256);

    /* 模块级过滤标志 */
    g_instr_module_flags = 0;
    if (ini_get_bool(cfg, "debug", "utsm_trace", 0))
        g_instr_module_flags |= INSTR_F_UTSM_TRACE;
    if (ini_get_bool(cfg, "debug", "vmm_trace", 0))
        g_instr_module_flags |= INSTR_F_VMM_TRACE;
    if (ini_get_bool(cfg, "debug", "dkm_trace", 0))
        g_instr_module_flags |= INSTR_F_DKM_TRACE;
    if (ini_get_bool(cfg, "debug", "dsk_trace", 0))
        g_instr_module_flags |= INSTR_F_DSK_TRACE;
    if (ini_get_bool(cfg, "debug", "block_trace", 0))
        g_instr_module_flags |= INSTR_F_BLOCK_TRACE;
    if (ini_get_bool(cfg, "debug", "net_trace", 0))
        g_instr_module_flags |= INSTR_F_NET_TRACE;
    if (ini_get_bool(cfg, "debug", "input_trace", 0))
        g_instr_module_flags |= INSTR_F_INPUT_TRACE;

    /* 校准 TSC（独立于 serial.c 的校准，供插桩时间戳使用） */
    instr_tsc_calibrate();

    /* 初始化探测缓冲 */
    probe_init(cfg);

    /* 输出初始化状态 */
    if (g_instr_enabled) {
        log_info("[INSTR] enabled");
        log_hex64("[INSTR] log_level=", g_instr_log_level);
        log_hex64("[INSTR] probe_enable=", g_instr_probe_enable);
        log_hex64("[INSTR] module_flags=", g_instr_module_flags);
        log_hex64("[INSTR] tsc_per_ms=", instr_tsc_per_ms);
    }
}
