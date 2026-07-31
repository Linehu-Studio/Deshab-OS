/* instr.h — Deshab 统一插桩宏系统
 *
 * 提供四级插桩原语:
 *   INSTR_LOG   — 带级别和模块过滤的日志输出
 *   INSTR_STAT  — 轻量级计数器 + 周期性摘要
 *   INSTR_PROBE — 环形探测缓冲写入（结构化事件记录）
 *   INSTR_TS    — 时间戳标注（阶段耗时测量）
 *
 * 设计原则:
 *   - 禁用时零开销: g_instr_enabled=0 时所有宏展开为空操作
 *   - 热路径安全: STAT_INC 仅一条条件自增，PROBE 仅条件写入
 *   - 统一配置: 通过 FUCK [debug] 段控制开关与粒度
 *   - 标签编码: 4 字符 ASCII 编码为 u32，避免字符串比较
 *
 * 使用方式:
 *   1. 在 kernel_main() 中调用 instr_init(&g_boot_cfg) 初始化
 *   2. 各模块 #include <utsm/instr.h> 后使用 INSTR_* 宏
 *   3. 在 FUCK 中设置 [debug] enabled=1 开启插桩
 */
#ifndef UTSM_INSTR_H
#define UTSM_INSTR_H

#include <utsm/types.h>

/* ---- 日志级别 ---- */
#define INSTR_LOG_TRACE  0
#define INSTR_LOG_DEBUG  1
#define INSTR_LOG_INFO   2
#define INSTR_LOG_WARN   3
#define INSTR_LOG_ERROR  4
#define INSTR_LOG_SILENT 5

/* ---- 全局配置变量 (instr.c 定义) ---- */
extern u32 g_instr_enabled;         /* 全局插桩开关，FUCK debug.enabled */
extern u32 g_instr_log_level;       /* 日志级别阈值，FUCK debug.log_level */
extern u32 g_instr_probe_enable;    /* 探测缓冲开关，FUCK debug.probe_enable */
extern u32 g_instr_stat_interval;   /* 统计摘要间隔，FUCK debug.stat_summary_interval */
extern u32 g_instr_module_flags;    /* 模块级过滤位掩码 */

/* ---- 模块标志位 ---- */
#define INSTR_F_UTSM_TRACE   (1u << 0)
#define INSTR_F_VMM_TRACE    (1u << 1)
#define INSTR_F_DKM_TRACE    (1u << 2)
#define INSTR_F_DSK_TRACE    (1u << 3)
#define INSTR_F_BLOCK_TRACE  (1u << 4)
#define INSTR_F_NET_TRACE    (1u << 5)
#define INSTR_F_INPUT_TRACE  (1u << 6)

/* ---- INSTR_LOG: 带级别和模块过滤的日志 ----
 * level:    INSTR_LOG_TRACE/DEBUG/INFO/WARN/ERROR
 * mod_flag: INSTR_F_xxx 或 0(不过滤)
 * msg:      常量字符串
 *
 * 仅当 g_instr_enabled && level >= g_instr_log_level &&
 *       (mod_flag==0 || mod_flag & g_instr_module_flags) 时输出。
 */
#define INSTR_LOG(level, mod_flag, msg) do { \
    if (g_instr_enabled && (level) >= g_instr_log_level && \
        ((mod_flag) == 0 || (g_instr_module_flags & (mod_flag)))) { \
        if ((level) <= INSTR_LOG_ERROR) log_error(msg); \
        else if ((level) <= INSTR_LOG_WARN) log_warn(msg); \
        else log_info(msg); \
    } \
} while(0)

/* ---- INSTR_STAT: 轻量级计数器 (声明+增量+摘要) ----
 * INSTR_STAT_DECL(name) — 在函数/文件作用域声明计数器
 * INSTR_STAT_INC(name)  — 原子性自增 (仅当 enabled)
 * INSTR_STAT_SUMMARY(name) — 达到间隔时输出摘要并重置
 */
#define INSTR_STAT_DECL(name) \
    static u64 name##_count = 0; \
    static u64 name##_last_ts __attribute__((unused)) = 0

#define INSTR_STAT_INC(name) do { \
    if (g_instr_enabled) { name##_count++; } \
} while(0)

#define INSTR_STAT_SUMMARY(name) do { \
    if (g_instr_enabled && name##_count >= g_instr_stat_interval) { \
        u64 _now = instr_rdtsc(); \
        u64 _elapsed_ms = (_now - name##_last_ts) / (instr_tsc_per_ms ? instr_tsc_per_ms : 1); \
        log_info("[STAT] " #name " count="); \
        log_hex64("", name##_count); \
        log_info("[STAT] " #name " interval_ms="); \
        log_hex64("", _elapsed_ms); \
        name##_count = 0; \
        name##_last_ts = _now; \
    } \
} while(0)

/* ---- INSTR_PROBE: 写入环形探测缓冲 ----
 * 标签编码: 4 字符 ASCII，如 "BOOT", "VME0", "EPTV", "IDTI", "AHCI"
 * a0-a3: 4 个 u64 参数，语义由标签定义
 *
 * 仅当 g_instr_enabled && g_instr_probe_enable 时写入。
 */
#define INSTR_PROBE(tag, a0, a1, a2, a3) do { \
    if (g_instr_enabled && g_instr_probe_enable) \
        probe_write(INSTR_TAG_##tag, (u64)(a0), (u64)(a1), (u64)(a2), (u64)(a3)); \
} while(0)

/* ---- INSTR_TS: 时间戳标注（阶段耗时测量） ----
 * INSTR_TS_DECL(name) — 声明时间戳变量
 * INSTR_TS_BEGIN(name) — 记录起始 TSC
 * INSTR_TS_END(name, label) — 计算耗时并输出（仅当 enabled）
 */
#define INSTR_TS_DECL(name) static u64 name##_ts = 0

#define INSTR_TS_BEGIN(name) do { name##_ts = instr_rdtsc(); } while(0)

#define INSTR_TS_END(name, label) do { \
    if (g_instr_enabled) { \
        u64 _d = instr_rdtsc() - name##_ts; \
        u64 _ms = _d / (instr_tsc_per_ms ? instr_tsc_per_ms : 1); \
        log_info("[TS] " label " elapsed_ms="); \
        log_hex64("", _ms); \
    } \
} while(0)

/* ---- 标签编码常量 ----
 * 4 字符 ASCII 编码为大端序 u32:
 *   'B'=0x42, 'O'=0x4F, 'O'=0x4F, 'T'=0x54 → 0x424F4F54
 */
#define INSTR_TAG_BOOT  0x424F4F54u  /* "BOOT" — 通用启动事件 */
#define INSTR_TAG_IDT0  0x49445430u  /* "IDT0" — IDT/中断初始化 */
#define INSTR_TAG_ARE0  0x41524530u  /* "ARE0" — Arena 初始化 */
#define INSTR_TAG_DMA0  0x444D4130u  /* "DMA0" — DMA 初始化 */
#define INSTR_TAG_NET0  0x4E455430u  /* "NET0" — 网络注册 */
#define INSTR_TAG_DRR0  0x44525230u  /* "DRR0" — DRR 初始化 */
#define INSTR_TAG_UTS0  0x55545330u  /* "UTS0" — UTSM 核心初始化 */
#define INSTR_TAG_DKM0  0x444B4D30u  /* "DKM0" — DKM 加载 */
#define INSTR_TAG_DSM0  0x44534D30u  /* "DSM0" — 驱动 manifest 加载 */
#define INSTR_TAG_DSK0  0x44534B30u  /* "DSK0" — DSK 加载跳转 */
#define INSTR_TAG_SELF  0x53454C46u  /* "SELF" — 自检 */
#define INSTR_TAG_VMM0  0x564D4D30u  /* "VMM0" — VMM 初始化 */
#define INSTR_TAG_LIN0  0x4C494E30u  /* "LIN0" — Linux loader */
#define INSTR_TAG_VME0  0x564D4530u  /* "VME0" — VM-Exit 事件 */
#define INSTR_TAG_EPTV  0x45505456u  /* "EPTV" — EPT violation */
#define INSTR_TAG_EPTM  0x4550544Du  /* "EPTM" — EPT misconfig */
#define INSTR_TAG_AHCI  0x41484349u  /* "AHCI" — AHCI 事件 */
#define INSTR_TAG_E1K0  0x45314B30u  /* "E1K0" — e1000 事件 */
#define INSTR_TAG_FAT0  0x46415430u  /* "FAT0" — FAT32 事件 */
#define INSTR_TAG_NVM0  0x4E564D30u  /* "NVM0" — NVMe 事件 */
#define INSTR_TAG_IRQ0  0x49525130u  /* "IRQ0" — IRQ 注册/触发 */
#define INSTR_TAG_PIT0  0x50495430u  /* "PIT0" — PIT 模拟 */
#define INSTR_TAG_SRL0  0x53524C30u  /* "SRL0" — 串口事件 */
#define INSTR_TAG_MMIO  0x4D4D494Fu  /* "MMIO" — MMIO 访问 */
#define INSTR_TAG_HCALL 0x4843414Cu  /* "HCAL" — Hypercall */
#define INSTR_TAG_EXCP  0x45584350u  /* "EXCP" — CPU 异常 */

/* ---- 探测条目结构（与 probe.c 共享） ---- */
typedef struct probe_entry {
    u32 tag;        /* 4字符ASCII标签 */
    u32 tick;       /* 低32位TSC（~2s溢出@2GHz，足够诊断） */
    u64 args[4];    /* 4个u64参数 */
} probe_entry;

/* ---- 辅助函数声明 (instr.c / probe.c 定义) ---- */
u64  instr_rdtsc(void);
extern u64 instr_tsc_per_ms;
void instr_init(const void *ini_config);

void probe_init(const void *ini_config);
void probe_write(u32 tag, u64 a0, u64 a1, u64 a2, u64 a3);
int  probe_read(u32 n, probe_entry *out);
u32  probe_count(void);
u32  probe_capacity(void);
probe_entry *probe_buf_address(void);
u32  probe_head_value(void);
u32  probe_dropped_count(void);

/* 需要前向声明 log 函数 (避免循环 include) */
#ifndef UTSM_LOG_H
void log_info(const char *msg);
void log_warn(const char *msg);
void log_error(const char *msg);
void log_hex64(const char *prefix, u64 value);
#endif

#endif /* UTSM_INSTR_H */
