/* dkm_instr.h — DKM 驱动专用插桩宏（无 UTSM 头文件依赖）
 *
 * 设计原则:
 *   - 使用 dkm_shared.h 已有的 dkm_rdtsc()/dkm_tsc_per_ms
 *   - 日志通过 dkm_kernel_api->log 输出
 *   - 统计计数器是 file-local static
 *   - 禁用时全部宏展开为空（零开销）
 *   - 需在 driver_init 开头调用 dkm_instr_init(api) 初始化
 *
 * 使用方式:
 *   1. #include "../dkm_instr.h"
 *   2. 在 driver_init 开头: dkm_instr_init(api);
 *   3. 使用 DKM_STAT_INC / DKM_TS / DKM_LOG_TRACE 宏
 */
#ifndef DKM_INSTR_H
#define DKM_INSTR_H

#include "dkm_shared.h"

/* ---- 全局开关（dkm_instr.c 定义，每个 .drv 引用同一符号） ----
 * 注意: DKM .drv 是 ET_REL 可重定位对象，每个驱动有独立的数据段副本。
 * 因此 dkm_instr_global_enable 在每个 .drv 中独立控制，由
 * dkm_instr_init() 在 driver_init 中根据 kernel_api 设置。
 */
static u32 dkm_instr_global_enable __attribute__((unused)) = 0;

/* ---- DKM_STAT: 轻量级计数器 ----
 * DKM_STAT_DECL(name) — 在函数/文件作用域声明计数器
 * DKM_STAT_INC(name)  — 自增（仅当 enabled）
 * DKM_STAT_DUMP(name, log_info_fn) — 输出当前计数值
 */
#define DKM_STAT_DECL(name) static u64 name##_count = 0

#define DKM_STAT_INC(name) do { \
    if (dkm_instr_global_enable) { name##_count++; } \
} while(0)

#define DKM_STAT_DUMP(name, log_info_fn) do { \
    if (dkm_instr_global_enable && name##_count > 0) { \
        log_info_fn("[DKM:STAT] " #name " count="); \
        /* 简单 u64 to hex 串口输出 */ \
        static const char _hx[] = "0123456789abcdef"; \
        char _buf[19]; _buf[0]='0'; _buf[1]='x'; \
        for(int _i=0;_i<16;_i++) _buf[2+_i]=_hx[(name##_count>>((15-_i)*4))&0xf]; \
        _buf[18]=0; log_info_fn(_buf); \
    } \
} while(0)

/* ---- DKM_TS: 时间戳测量 ----
 * DKM_TS_DECL(name) — 声明时间戳变量
 * DKM_TS_BEGIN(name) — 记录起始 TSC
 * DKM_TS_END(name, log_info_fn, label) — 计算耗时并输出
 */
#define DKM_TS_DECL(name) static u64 name##_ts = 0

#define DKM_TS_BEGIN(name) do { name##_ts = dkm_rdtsc(); } while(0)

#define DKM_TS_END(name, log_info_fn, label) do { \
    if (dkm_instr_global_enable && dkm_tsc_per_ms) { \
        u64 _d = dkm_rdtsc() - name##_ts; \
        u64 _ms = _d / dkm_tsc_per_ms; \
        log_info_fn("[DKM:TS] " label " elapsed_ms="); \
        static const char _hx[] = "0123456789abcdef"; \
        char _buf[19]; _buf[0]='0'; _buf[1]='x'; \
        for(int _i=0;_i<16;_i++) _buf[2+_i]=_hx[(_ms>>((15-_i)*4))&0xf]; \
        _buf[18]=0; log_info_fn(_buf); \
    } \
} while(0)

/* ---- DKM_LOG_TRACE: 调试级别日志 ----
 * 仅当插桩启用时通过 api->log->info 输出
 */
#define DKM_LOG_TRACE(log_api, msg) do { \
    if (dkm_instr_global_enable && (log_api)) (log_api)->info(msg); \
} while(0)

/* ---- 初始化：读取 kernel_api 中的插桩标志 ----
 * kernel_api->feature_bits bit0 = 插桩启用标志
 * 由 UTSM dkm_fill_platform_info() 根据 FUCK debug.dkm_trace 设置
 */
static __inline__ void dkm_instr_init(const struct dkm_kernel_api *api) {
    if (api && (api->feature_bits & 0x01)) {
        dkm_instr_global_enable = 1;
    }
}

#endif /* DKM_INSTR_H */
