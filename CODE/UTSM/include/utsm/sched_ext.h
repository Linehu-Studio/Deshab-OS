#ifndef UTSM_SCHED_EXT_H
#define UTSM_SCHED_EXT_H

#include <utsm/types.h>
#include <utsm/sched.h>

/*
 * SAS-R0-PCQ 调度器扩展接口（ROADMAP Phase 7）。
 *
 * 冻结 ABI（sched.h）之外的内核本体能力层，仅供 UTSM 内核与 selftest
 * 使用，不跨组件边界（DKM 驱动经 kernel_api.sched 使用精简服务表）。
 *
 * 切换模型（xv6 式栈驱动）：
 *   - 新任务初始栈帧由 utsm_task_create 伪造（7 个 u64）：
 *       sp[0..3]=r15/r14/r13/r12(0), sp[4]=rbx(task slot), sp[5]=rbp(0),
 *       sp[6]=&utsm_task_trampoline —— switch 的 ret 直接落 trampoline。
 *   - 抢占：IRQ 帧落在被抢占任务自己的栈上，切换只换 rsp（arch.rsp）；
 *     恢复时沿原栈 iretq。TCB arch.rip/rflags 仅占位零（resume 全由 rsp 驱动）。
 *   - 协作：schedule() 入口 pushfq 保存本任务 rflags → cli；恢复点 popfq。
 */

/* 应用 FUCK [sched] 配置（enable 前生效；tick_hz=0 用默认 100） */
void utsm_sched_config(u32 tick_hz, u32 timeslice_ms, u32 demo_ms, int demo_fault);

/* bootstrap 任务绑定：sched_init + cpu_init(0) + 当前 rsp 注册为 RUNNING
 * 任务（priority 最低，K_KERNEL|NO_MIGRATE）。不开中断、不碰定时器。
 * 返回 bootstrap slot（>=0），负数失败。 */
int utsm_sched_bootstrap_init(void);

/* 创建任务：4 页任务栈（arena kmem_alloc_page）+ 伪造初始栈帧 + 入队。
 * process_slot = 0xFFFFFFFF 表示无进程关联（纯内核任务，crypto_context=NULL）。
 * 返回 task slot（>=0），负数失败。仅在未启用抢占时（bootstrap 上下文）调用。 */
int utsm_task_create(u32 process_slot, u32 priority, void (*entry)(void *), void *arg);

/* 启用抢占：LAPIC timer 校准配置 → 注册 tick handler → 异常钩子 →
 * DRR fault 回调接线 → LVT 解除 mask。tick_hz=0 用配置默认。
 * 返回 0 成功；失败时调度器保持协作模式（系统继续正常启动）。 */
int utsm_sched_enable(u32 tick_hz);

/* 静默：cli → 停 LAPIC timer → 异常钩子注销 → DRR 回调解绑 → 杀掉全部
 * 非 bootstrap 任务 → sti。dsk_load_and_jump 之前调用，保证 DSK 交接
 * 路径与无调度器时逐字节一致。 */
void utsm_sched_quiesce(void);

/* demo 窗口：创建 3 个不同优先级 demo 任务（prio 1/20/40）→ enable →
 * bootstrap 忙等 demo_ms 毫秒（被周期抢占，demo 任务交错运行 = 抢占证据）。
 * FUCK [sched] demo_ms=0 时为空操作。 */
void utsm_sched_demo_window(void);

/* 协作让出（当前任务重新入队并切换） */
void utsm_sched_yield(void);

/* 睡眠 ms 毫秒（wake_deadline 到期后由 tick 唤醒） */
void utsm_sched_sleep_ms(u32 ms);

/* 任务退出（trampoline 在 entry 返回后自动调用；不返回） */
void utsm_sched_task_exit(void) __attribute__((noreturn));

/* DRR fault 回调：杀掉指定任务（crypto erase + FAULTED→ZOMBIE）。
 * 若 fault 任务是当前任务（tick/异常上下文）则立即切换走且不返回。 */
void utsm_sched_fault_task(u32 task_slot);

/* task_kill（ROADMAP Phase7 验收项）：crypto erase（key_epoch++）+
 * 摘链 + ZOMBIE。对 RUNNING（当前）任务返回 ERR_STATE。 */
int utsm_sched_task_kill(u32 task_slot);

/* 当前运行任务 slot */
u32 utsm_sched_current_slot(void);

/* ---- switch.S 汇编符号 ----
 * utsm_sched_switch: 保存 prev 的 callee-saved + arch.rsp，载入 next->arch.rsp
 * 恢复并 ret。调用后 volatile 视为全部破坏（C 调用约定保证）。 */
void utsm_sched_switch(utsm_sched_tcb *prev, utsm_sched_tcb *next);
void utsm_task_trampoline(void);

/* trampoline 的 C 入口（rbx 传入 task slot） */
void utsm_task_main(u64 slot);

/* ---- selftest 钩子（内部状态观察，仅 test/ 使用） ---- */
int utsm_sched_selftest_runqueue(void);
int utsm_sched_selftest_blockwake(void);
int utsm_sched_debug_ready_count(u32 cpu_id);

#endif /* UTSM_SCHED_EXT_H */
