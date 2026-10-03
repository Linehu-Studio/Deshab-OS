/* sched.c — SAS-R0-PCQ 调度器实现（ROADMAP Phase 7）
 *
 * 冻结 ABI（include/utsm/sched.h）9 个 API 的真实现 + 内核扩展层
 * （include/utsm/sched_ext.h）。
 *
 * 设计要点：
 *   - O(1) 热路径：ready_bitmap ctz 选最高优先级（0 最高），head[] 取出
 *     即出队；enqueue/dequeue 均为 O(1) 侵入式 FIFO（slot 索引非指针）。
 *   - 抢占 = xv6 式栈驱动切换：IRQ 帧落在被抢占任务自己的栈上，切换只换
 *     rsp（arch.rsp）；恢复时沿原栈 iretq。TCB arch.rip/rflags 占位零。
 *   - tick 源 = LAPIC timer（宿主自持，见 arch/x86_64/lapic_timer.c）。
 *   - 本迭代 BSP-only：runqueue 数组结构就位，仅用 cpu0。
 *   - 切换路径只触碰 crypto_context 指针；禁止扫描 capability / 计算
 *     MAC / 写 checkpoint（设计约束，见 docs/RE/UTSM_算法记录.md §10）。
 */

#include <utsm/sched.h>
#include <utsm/sched_ext.h>
#include <utsm/log.h>
#include <utsm/panic.h>
#include <utsm/arena.h>
#include <utsm/config.h>
#include <utsm/process.h>
#include <utsm/idt.h>
#include <utsm/drr.h>
#include <utsm/lapic_timer.h>
#include <utsm/dkm.h>

void utsm_sched_switch(utsm_sched_tcb *prev, utsm_sched_tcb *next); /* switch.S */
void utsm_task_trampoline(void);                                    /* switch.S */

#define UTSM_SCHED_MAX_TASKS 64
#define UTSM_SCHED_MAX_CPUS  8
#define UTSM_SCHED_NO_PROCESS 0xFFFFFFFFu

struct task_aux {
    void (*entry)(void *);
    void *arg;
    u64 stack_base;
    u64 run_start_ns;
};

static utsm_sched_tcb       g_task_table[UTSM_SCHED_MAX_TASKS];
static utsm_sched_run_queue g_runqueues[UTSM_SCHED_MAX_CPUS];
static utsm_sched_run_queue *g_rq_ptrs[UTSM_SCHED_MAX_CPUS];
static struct task_aux      g_task_aux[UTSM_SCHED_MAX_TASKS];

static volatile int g_sched_enabled = 0;
static volatile int g_switch_lock   = 0;
static u64 g_uptime_ns      = 0;
static u64 g_tick_ns        = 10ULL * 1000ULL * 1000ULL;   /* 100Hz */
static u64 g_timeslice_ns   = 10ULL * 1000ULL * 1000ULL;   /* 10ms */
static u32 g_cfg_tick_hz    = 100;
static u32 g_demo_ms        = 1000;
static int g_demo_fault     = 0;
static int g_wake_pending   = 0;
static u32 g_bootstrap_slot = UTSM_SCHED_INVALID_SLOT;

/* ---- switch.S 依赖的 TCB/arch 布局断言 ---- */
#define OFFS(t, m) ((u32)__builtin_offsetof(t, m))
_Static_assert(OFFS(utsm_sched_tcb, arch) == 80,                 "switch.S: arch@80");
_Static_assert(OFFS(utsm_sched_arch_context, rsp) == 0,          "switch.S: rsp@0");
_Static_assert(OFFS(utsm_sched_arch_context, rbx) == 24,         "switch.S: rbx@24");
_Static_assert(OFFS(utsm_sched_arch_context, rbp) == 32,         "switch.S: rbp@32");
_Static_assert(OFFS(utsm_sched_arch_context, r12) == 40,         "switch.S: r12@40");
_Static_assert(OFFS(utsm_sched_arch_context, r15) == 64,         "switch.S: r15@64");
_Static_assert(UTSM_SCHED_PRIORITY_LOWEST == 63,                 "switch/bitmap: 64 prio");

static u32 task_slot_of(const utsm_sched_tcb *t) {
    return (u32)(t - g_task_table);
}

/* ===================================================================
 *  冻结 ABI 实现（sched.h）
 * =================================================================== */

int utsm_sched_init(void) {
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        utsm_sched_tcb *t = &g_task_table[i];
        u64 *w = (u64 *)t;
        for (u64 k = 0; k < sizeof(*t) / sizeof(u64); k++) w[k] = 0;
        t->abi_version = UTSM_SCHED_ABI_VERSION;
        t->struct_size = (u32)sizeof(utsm_sched_tcb);
        t->state = UTSM_SCHED_TASK_UNUSED;
        t->runq_prev = UTSM_SCHED_INVALID_SLOT;
        t->runq_next = UTSM_SCHED_INVALID_SLOT;
        g_task_aux[i].entry = 0;
        g_task_aux[i].arg = 0;
        g_task_aux[i].stack_base = 0;
        g_task_aux[i].run_start_ns = 0;
    }
    for (u32 c = 0; c < UTSM_SCHED_MAX_CPUS; c++) g_rq_ptrs[c] = &g_runqueues[c];
    g_sched_enabled = 0;
    g_uptime_ns = 0;
    g_bootstrap_slot = UTSM_SCHED_INVALID_SLOT;
    return UTSM_SCHED_OK;
}

int utsm_sched_cpu_init(u32 cpu_id, utsm_sched_run_queue *queue) {
    if (cpu_id >= UTSM_SCHED_MAX_CPUS) return UTSM_SCHED_ERR_INVALID;
    utsm_sched_run_queue *rq = queue ? queue : &g_runqueues[cpu_id];
    u64 *w = (u64 *)rq;
    for (u64 k = 0; k < sizeof(*rq) / sizeof(u64); k++) w[k] = 0;
    rq->abi_version = UTSM_SCHED_ABI_VERSION;
    rq->struct_size = (u32)sizeof(utsm_sched_run_queue);
    rq->cpu_id = cpu_id;
    for (u32 p = 0; p < UTSM_SCHED_PRIORITY_COUNT; p++) {
        rq->head[p] = UTSM_SCHED_INVALID_SLOT;
        rq->tail[p] = UTSM_SCHED_INVALID_SLOT;
    }
    rq->current_task_slot = UTSM_SCHED_INVALID_SLOT;
    rq->idle_task_slot = UTSM_SCHED_INVALID_SLOT;
    rq->ready_count = 0;
    g_rq_ptrs[cpu_id] = rq;
    return UTSM_SCHED_OK;
}

int utsm_sched_task_init(utsm_sched_tcb *task, u64 task_id,
                         u32 process_slot, u32 priority, u64 affinity_mask) {
    if (!task || priority >= UTSM_SCHED_PRIORITY_COUNT) return UTSM_SCHED_ERR_INVALID;
    u32 slot = task_slot_of(task);
    if (slot >= UTSM_SCHED_MAX_TASKS) return UTSM_SCHED_ERR_INVALID;
    if (task->state != UTSM_SCHED_TASK_UNUSED) return UTSM_SCHED_ERR_STATE;
    task->task_id = task_id;
    task->process_slot = process_slot;
    task->flags = 0;
    task->state = UTSM_SCHED_TASK_NEW;
    task->priority = priority;
    task->assigned_cpu = 0;
    task->last_cpu = 0;
    task->affinity_mask = affinity_mask;
    task->timeslice_ns = 0;
    task->wake_deadline_ns = 0;
    task->crypto_context = 0;
    task->runq_prev = UTSM_SCHED_INVALID_SLOT;
    task->runq_next = UTSM_SCHED_INVALID_SLOT;
    task->arch.rsp = 0;
    task->arch.rip = 0;
    task->arch.rflags = 0;
    task->arch.rbx = 0;
    task->arch.rbp = 0;
    task->arch.r12 = 0;
    task->arch.r13 = 0;
    task->arch.r14 = 0;
    task->arch.r15 = 0;
    return UTSM_SCHED_OK;
}

static void rq_link(utsm_sched_run_queue *rq, utsm_sched_tcb *t) {
    u32 slot = task_slot_of(t);
    u32 p = t->priority;
    t->runq_next = UTSM_SCHED_INVALID_SLOT;
    if (rq->head[p] == UTSM_SCHED_INVALID_SLOT) {
        t->runq_prev = UTSM_SCHED_INVALID_SLOT;
        rq->head[p] = slot;
        rq->tail[p] = slot;
        rq->ready_bitmap |= 1ULL << p;
    } else {
        u32 tail = rq->tail[p];
        t->runq_prev = tail;
        g_task_table[tail].runq_next = slot;
        rq->tail[p] = slot;
    }
    rq->ready_count++;
}

static void rq_unlink(utsm_sched_run_queue *rq, utsm_sched_tcb *t) {
    u32 slot = task_slot_of(t);
    u32 p = t->priority;
    u32 prev = t->runq_prev;
    u32 next = t->runq_next;
    if (prev != UTSM_SCHED_INVALID_SLOT) g_task_table[prev].runq_next = next;
    if (next != UTSM_SCHED_INVALID_SLOT) g_task_table[next].runq_prev = prev;
    if (rq->head[p] == slot) rq->head[p] = next;
    if (rq->tail[p] == slot) rq->tail[p] = prev;
    if (rq->head[p] == UTSM_SCHED_INVALID_SLOT) {
        rq->ready_bitmap &= ~(1ULL << p);
    }
    t->runq_prev = UTSM_SCHED_INVALID_SLOT;
    t->runq_next = UTSM_SCHED_INVALID_SLOT;
    if (rq->ready_count > 0) rq->ready_count--;
}

int utsm_sched_enqueue(u32 cpu_id, utsm_sched_tcb *task) {
    if (!task || cpu_id >= UTSM_SCHED_MAX_CPUS) return UTSM_SCHED_ERR_INVALID;
    if (task->state == UTSM_SCHED_TASK_READY ||
        task->state == UTSM_SCHED_TASK_RUNNING) {
        return UTSM_SCHED_ERR_STATE;
    }
    task->assigned_cpu = cpu_id;
    task->state = UTSM_SCHED_TASK_READY;
    rq_link(g_rq_ptrs[cpu_id], task);
    return UTSM_SCHED_OK;
}

int utsm_sched_dequeue(utsm_sched_tcb *task) {
    if (!task) return UTSM_SCHED_ERR_INVALID;
    if (task->state != UTSM_SCHED_TASK_READY) return UTSM_SCHED_OK;  /* 幂等 */
    rq_unlink(g_rq_ptrs[task->assigned_cpu], task);
    return UTSM_SCHED_OK;
}

int utsm_sched_block(utsm_sched_tcb *task, u32 blocked_state) {
    if (!task) return UTSM_SCHED_ERR_INVALID;
    if (blocked_state != UTSM_SCHED_TASK_BLOCKED &&
        blocked_state != UTSM_SCHED_TASK_SLEEPING) {
        return UTSM_SCHED_ERR_INVALID;
    }
    if (task->state == UTSM_SCHED_TASK_READY) {
        rq_unlink(g_rq_ptrs[task->assigned_cpu], task);
    } else if (task->state != UTSM_SCHED_TASK_RUNNING) {
        return UTSM_SCHED_ERR_STATE;
    }
    task->state = blocked_state;
    return UTSM_SCHED_OK;
}

int utsm_sched_wake(utsm_sched_tcb *task) {
    if (!task) return UTSM_SCHED_ERR_INVALID;
    if (task->state != UTSM_SCHED_TASK_BLOCKED &&
        task->state != UTSM_SCHED_TASK_SLEEPING) {
        return UTSM_SCHED_ERR_STATE;
    }
    return utsm_sched_enqueue(task->assigned_cpu, task);
}

/* pick 即出队；纯队列操作，不改 current_task_slot（selftest 依赖此语义） */
int utsm_sched_pick_next(u32 cpu_id, utsm_sched_tcb **out_task) {
    if (!out_task || cpu_id >= UTSM_SCHED_MAX_CPUS) return UTSM_SCHED_ERR_INVALID;
    utsm_sched_run_queue *rq = g_rq_ptrs[cpu_id];
    if (rq->ready_bitmap == 0) {
        *out_task = 0;
        return UTSM_SCHED_ERR_NO_TASK;
    }
    u32 p = (u32)__builtin_ctzll(rq->ready_bitmap);   /* 0 = 最高优先级 */
    u32 slot = rq->head[p];
    if (slot >= UTSM_SCHED_MAX_TASKS) {
        *out_task = 0;
        return UTSM_SCHED_ERR_NO_TASK;
    }
    utsm_sched_tcb *t = &g_task_table[slot];
    rq_unlink(rq, t);
    t->state = UTSM_SCHED_TASK_RUNNING;
    t->last_cpu = cpu_id;
    g_task_aux[slot].run_start_ns = g_uptime_ns;
    *out_task = t;
    return UTSM_SCHED_OK;
}

int utsm_sched_tick(u32 cpu_id, u64 elapsed_ns, utsm_sched_tcb **out_preempted_task) {
    if (out_preempted_task) *out_preempted_task = 0;
    g_wake_pending = 0;
    g_uptime_ns += elapsed_ns;

    /* 唤醒到期 sleeper（非热路径：任务数 <= 64 线性扫描） */
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        utsm_sched_tcb *t = &g_task_table[i];
        if (t->state == UTSM_SCHED_TASK_SLEEPING &&
            t->wake_deadline_ns != 0 && t->wake_deadline_ns <= g_uptime_ns) {
            t->wake_deadline_ns = 0;
            utsm_sched_wake(t);
            g_wake_pending = 1;
        }
    }

    utsm_sched_run_queue *rq = g_rq_ptrs[cpu_id];
    u32 cur_slot = rq->current_task_slot;
    if (cur_slot == UTSM_SCHED_INVALID_SLOT) return UTSM_SCHED_OK;
    utsm_sched_tcb *cur = &g_task_table[cur_slot];
    if (cur->state != UTSM_SCHED_TASK_RUNNING) return UTSM_SCHED_OK;
    u64 ran = g_uptime_ns - g_task_aux[cur_slot].run_start_ns;
    if (ran >= g_timeslice_ns && out_preempted_task) {
        *out_preempted_task = cur;
    }
    return UTSM_SCHED_OK;
}

/* ===================================================================
 *  内核扩展层（sched_ext.h）
 * =================================================================== */

void utsm_sched_config(u32 tick_hz, u32 timeslice_ms, u32 demo_ms, int demo_fault) {
    g_cfg_tick_hz = tick_hz ? tick_hz : 100;
    if (timeslice_ms) g_timeslice_ns = (u64)timeslice_ms * 1000000ULL;
    g_demo_ms = demo_ms;
    g_demo_fault = demo_fault;
}

int utsm_sched_bootstrap_init(void) {
    utsm_sched_init();
    utsm_sched_cpu_init(0, 0);

    u32 slot = UTSM_SCHED_MAX_TASKS;
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        if (g_task_table[i].state == UTSM_SCHED_TASK_UNUSED) { slot = i; break; }
    }
    if (slot == UTSM_SCHED_MAX_TASKS) return UTSM_SCHED_ERR_NO_TASK;

    utsm_sched_tcb *t = &g_task_table[slot];
    utsm_sched_task_init(t, 1, UTSM_SCHED_NO_PROCESS,
                         UTSM_SCHED_PRIORITY_LOWEST, ~0ULL);
    t->flags = UTSM_SCHED_TASK_F_KERNEL | UTSM_SCHED_TASK_F_NO_MIGRATE;
    t->state = UTSM_SCHED_TASK_RUNNING;
    u64 rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    t->arch.rsp = rsp;   /* bootstrap 复用 boot.S kernel_stack，不迁移栈 */

    g_task_aux[slot].entry = 0;
    g_task_aux[slot].arg = 0;
    g_task_aux[slot].stack_base = 0;
    g_task_aux[slot].run_start_ns = 0;
    g_rq_ptrs[0]->current_task_slot = slot;
    g_bootstrap_slot = slot;

    log_info("[SCHED] bootstrap bound");
    log_hex64("[SCHED]   slot=", slot);
    return (int)slot;
}

int utsm_task_create(u32 process_slot, u32 priority, void (*entry)(void *), void *arg) {
    if (!entry || priority >= UTSM_SCHED_PRIORITY_COUNT) return UTSM_SCHED_ERR_INVALID;

    u32 slot = UTSM_SCHED_MAX_TASKS;
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        if (g_task_table[i].state == UTSM_SCHED_TASK_UNUSED) { slot = i; break; }
    }
    if (slot == UTSM_SCHED_MAX_TASKS) return UTSM_SCHED_ERR_NO_TASK;

    void *stack = kmem_alloc_page(4);   /* 16KB 任务栈（arena，非 DMA 资源） */
    if (!stack) return UTSM_SCHED_ERR_NO_TASK;
    u64 top = ((u64)stack + 4 * UTSM_PAGE_SIZE) & ~15ULL;

    utsm_sched_tcb *t = &g_task_table[slot];
    int rc = utsm_sched_task_init(t, 0x1000ULL + slot, process_slot, priority, ~0ULL);
    if (rc != UTSM_SCHED_OK) return rc;
    t->flags = UTSM_SCHED_TASK_F_KERNEL;

    /* crypto_context 指针：切换路径唯一触碰的加密状态 */
    if (process_slot != UTSM_SCHED_NO_PROCESS) {
        utsm_process_context *proc = utsm_process_get(process_slot);
        if (proc) t->crypto_context = &proc->crypto_ctx;
    }

    /* 伪造初始栈帧（与 switch.S 的 push/pop 布局逐字节对应）：
     * sp[0]=r15 sp[1]=r14 sp[2]=r13 sp[3]=r12 sp[4]=rbx(slot) sp[5]=rbp
     * sp[6]=&trampoline；ret 后 rsp=top（16 对齐，满足 call ABI） */
    u64 *sp = (u64 *)(top - 56);
    sp[0] = 0;
    sp[1] = 0;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = (u64)slot;
    sp[5] = 0;
    sp[6] = (u64)&utsm_task_trampoline;
    t->arch.rsp = (u64)sp;

    g_task_aux[slot].entry = entry;
    g_task_aux[slot].arg = arg;
    g_task_aux[slot].stack_base = (u64)stack;
    g_task_aux[slot].run_start_ns = 0;

    rc = utsm_sched_enqueue(0, t);
    if (rc != UTSM_SCHED_OK) return rc;
    return (int)slot;
}

void utsm_task_main(u64 slot) {
    if (slot >= UTSM_SCHED_MAX_TASKS) {
        drr_system_rollback("scheduler: trampoline bad slot");
    }
    struct task_aux *aux = &g_task_aux[slot];
    void (*entry)(void *) = aux->entry;
    void *arg = aux->arg;
    if (entry) entry(arg);
    utsm_sched_task_exit();
    for (;;) { __asm__ volatile("cli; hlt"); }
}

void utsm_sched_schedule(void) {
    utsm_sched_run_queue *rq = g_rq_ptrs[0];
    u32 cur_slot = rq->current_task_slot;
    if (cur_slot == UTSM_SCHED_INVALID_SLOT) return;
    utsm_sched_tcb *cur = &g_task_table[cur_slot];

    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    g_switch_lock = 1;

    if (cur->state == UTSM_SCHED_TASK_RUNNING) {
        cur->state = UTSM_SCHED_TASK_READY;
        rq_link(rq, cur);
    }

    utsm_sched_tcb *next = 0;
    int rc = utsm_sched_pick_next(0, &next);
    if (rc != UTSM_SCHED_OK || !next) {
        if (cur->state == UTSM_SCHED_TASK_READY) {
            /* 仅当前任务可运行：取回继续 */
            rq_unlink(rq, cur);
            cur->state = UTSM_SCHED_TASK_RUNNING;
            g_task_aux[cur_slot].run_start_ns = g_uptime_ns;
            g_switch_lock = 0;
            __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
            return;
        }
        /* 当前任务不可运行且无任务可切换 → 恢复根接管 */
        g_switch_lock = 0;
        drr_system_rollback("scheduler: no runnable task");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    if (next == cur) {
        cur->state = UTSM_SCHED_TASK_RUNNING;
        g_task_aux[cur_slot].run_start_ns = g_uptime_ns;
        g_switch_lock = 0;
        __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
        return;
    }

    rq->current_task_slot = task_slot_of(next);
    g_switch_lock = 0;
    utsm_sched_switch(cur, next);   /* 切走；恢复点在下一行 */
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

void utsm_sched_yield(void) {
    utsm_sched_schedule();
}

void utsm_sched_sleep_ms(u32 ms) {
    utsm_sched_run_queue *rq = g_rq_ptrs[0];
    u32 cur_slot = rq->current_task_slot;
    if (cur_slot == UTSM_SCHED_INVALID_SLOT) return;
    utsm_sched_tcb *cur = &g_task_table[cur_slot];
    cur->wake_deadline_ns = g_uptime_ns + (u64)ms * 1000000ULL;
    utsm_sched_block(cur, UTSM_SCHED_TASK_SLEEPING);
    utsm_sched_schedule();
}

void utsm_sched_task_exit(void) {
    utsm_sched_run_queue *rq = g_rq_ptrs[0];
    u32 cur_slot = rq->current_task_slot;
    if (cur_slot == UTSM_SCHED_INVALID_SLOT) {
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    utsm_sched_tcb *cur = &g_task_table[cur_slot];

    /* crypto erase：推进纪元使该进程旧密文全部失效 */
    utsm_crypto_context *cc = (utsm_crypto_context *)cur->crypto_context;
    if (cc) cc->crypto_epoch++;
    drr_watchdog_unregister(cur_slot);   /* 防止看门狗对已退出任务误报 */
    cur->state = UTSM_SCHED_TASK_ZOMBIE;

    __asm__ volatile("cli" ::: "memory");
    utsm_sched_tcb *next = 0;
    if (utsm_sched_pick_next(0, &next) != UTSM_SCHED_OK || !next) {
        drr_system_rollback("scheduler: no task after exit");
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    rq->current_task_slot = task_slot_of(next);
    utsm_sched_switch(cur, next);   /* 不返回 */
    for (;;) { __asm__ volatile("cli; hlt"); }
}

int utsm_sched_task_kill(u32 task_slot) {
    if (task_slot >= UTSM_SCHED_MAX_TASKS) return UTSM_SCHED_ERR_INVALID;
    if (task_slot == g_bootstrap_slot) return UTSM_SCHED_ERR_STATE;
    utsm_sched_tcb *t = &g_task_table[task_slot];
    if (t->state == UTSM_SCHED_TASK_UNUSED || t->state == UTSM_SCHED_TASK_ZOMBIE) {
        return UTSM_SCHED_OK;
    }
    if (t->state == UTSM_SCHED_TASK_RUNNING) return UTSM_SCHED_ERR_STATE;
    utsm_crypto_context *cc = (utsm_crypto_context *)t->crypto_context;
    if (cc) cc->crypto_epoch++;
    drr_watchdog_unregister(task_slot);
    utsm_sched_dequeue(t);
    t->state = UTSM_SCHED_TASK_ZOMBIE;
    return UTSM_SCHED_OK;
}

void utsm_sched_fault_task(u32 task_slot) {
    if (task_slot >= UTSM_SCHED_MAX_TASKS) return;
    utsm_sched_tcb *t = &g_task_table[task_slot];
    if (t->state == UTSM_SCHED_TASK_UNUSED || t->state == UTSM_SCHED_TASK_ZOMBIE) return;

    utsm_crypto_context *cc = (utsm_crypto_context *)t->crypto_context;
    if (cc) cc->crypto_epoch++;
    drr_watchdog_unregister(task_slot);
    t->state = UTSM_SCHED_TASK_FAULTED;
    utsm_sched_dequeue(t);

    utsm_sched_run_queue *rq = g_rq_ptrs[0];
    if (rq->current_task_slot == task_slot) {
        /* fault 任务是当前任务（tick/异常上下文）：杀掉并切走，不返回 */
        t->state = UTSM_SCHED_TASK_ZOMBIE;
        __asm__ volatile("cli" ::: "memory");
        utsm_sched_tcb *next = 0;
        if (utsm_sched_pick_next(0, &next) != UTSM_SCHED_OK || !next) {
            drr_system_rollback("scheduler: no task after fault");
            for (;;) { __asm__ volatile("cli; hlt"); }
        }
        rq->current_task_slot = task_slot_of(next);
        utsm_sched_switch(t, next);
        for (;;) { __asm__ volatile("cli; hlt"); }
    }
    t->state = UTSM_SCHED_TASK_ZOMBIE;
}

u32 utsm_sched_current_slot(void) {
    return g_rq_ptrs[0]->current_task_slot;
}

/* ===================================================================
 *  tick / 异常钩子 / enable / quiesce / demo
 * =================================================================== */

static int utsm_tick_handler(u8 vector) {
    (void)vector;
    lapic_timer_eoi();          /* EOI 先行：切换不得悬挂 in-service 位 */
    drr_heartbeat_tick();
    if (!g_sched_enabled) return 1;
    drr_watchdog_check();       /* 可能杀当前任务并切换（不返回） */
    utsm_sched_tcb *preempted = 0;
    utsm_sched_tick(0, g_tick_ns, &preempted);
    if (preempted || g_wake_pending) {
        utsm_sched_schedule();
    }
    return 1;                   /* 自负 EOI 全责（idt.c 约定） */
}

static int utsm_sched_exc_hook(u64 vector, u64 err, u64 rip) {
    (void)err;
    if (!g_sched_enabled) return 0;   /* 原路径：全量日志 + 停机 */
    u32 cur = g_rq_ptrs[0]->current_task_slot;
    if (cur == UTSM_SCHED_INVALID_SLOT || cur == g_bootstrap_slot) return 0;
    /* 任务级 fault：DRR 接管（页级回滚 + 杀任务 + 切换），本函数不返回 */
    log_error("[SCHED] task fault");
    log_hex64("[SCHED]   vector=", vector);
    log_hex64("[SCHED]   rip=", rip);
    drr_handle_task_fault(cur, "cpu exception");
    return 1;   /* 不可达：fault 路径已切换任务或 system rollback */
}

int utsm_sched_enable(u32 tick_hz) {
    if (tick_hz == 0) tick_hz = g_cfg_tick_hz;
    g_tick_ns = 1000000000ULL / tick_hz;

    if (lapic_timer_init(tick_hz) != 0) {
        log_error("[SCHED] lapic timer init failed, stays cooperative");
        panic_full("SAS-E04 LAPIC TIMER INIT FAILED",
                   "utsm_sched_enable: lapic_timer_init failed", 0);
        return -1;
    }
    int vec = lapic_timer_vector();
    if (vec < 0 || irq_register((u8)vec, utsm_tick_handler) != 0) {
        lapic_timer_stop();
        log_error("[SCHED] tick handler registration failed");
        panic_full("SAS-E05 TICK HANDLER REGISTER FAILED",
                   "utsm_sched_enable: tick handler registration failed", 0);
        return -2;
    }
    idt_set_exception_hook(utsm_sched_exc_hook);
    drr_set_sched_fault_cb(utsm_sched_fault_task);
    lapic_timer_start();        /* handler 就位后再放开投递 */
    g_sched_enabled = 1;
    log_info("[SCHED] enable (LAPIC timer)");
    log_hex64("[SCHED]   tick_hz=", tick_hz);
    return 0;
}

void utsm_sched_quiesce(void) {
    __asm__ volatile("cli" ::: "memory");
    g_sched_enabled = 0;
    idt_set_exception_hook(0);          /* 恢复 DSK 阶段异常诊断行为 */
    drr_set_sched_fault_cb(0);
    lapic_timer_stop();
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        if (i == g_bootstrap_slot) continue;
        utsm_sched_tcb *t = &g_task_table[i];
        if (t->state == UTSM_SCHED_TASK_UNUSED || t->state == UTSM_SCHED_TASK_ZOMBIE) continue;
        utsm_crypto_context *cc = (utsm_crypto_context *)t->crypto_context;
        if (cc) cc->crypto_epoch++;
        utsm_sched_dequeue(t);
        t->state = UTSM_SCHED_TASK_ZOMBIE;
    }
    log_info("[SCHED] quiesce");
    log_info("[SCHED] timer stopped");
    /* BUG-GP-IRET: 保持 IF=0 交接不变式（恢复漂移前行为）。
     * VM-exit 后 host RFLAGS=0x2（IF=0），DSM/selftest/DSK 交接全程 IF=0；
     * 此处 sti 会让 apic_route_legacy=1 放开的 IRQ1/11/12 在 LAPIC IRR
     * 挂起的中断于 dsk_load_and_jump 首条日志（log_emit popfq 恢复 IF=1）
     * 之后立即投递，其 handler 返回的 iretq 触发 #GP(err=0)。DSK 加载
     * 失败路径的日志在 IF=0 下同样可用（log_emit 不依赖 IF）。 */
    __asm__ volatile("cli" ::: "memory");
}

/* ---- demo 任务（抢占证据：日志交错 + 看门狗负向演练） ---- */

static void demo_task_body(void *arg) {
    u64 id = (u64)arg;   /* 1=A, 2=B, 3=C */
    static const char *const born_msg[4] = { "", "[TASK A] born", "[TASK B] born", "[TASK C] born" };
    static const char *const iter_msg[4] = { "", "[TASK A] iter=", "[TASK B] iter=", "[TASK C] iter=" };
    static const char *const prio_msg[4] = { "", "[TASK A] prio=", "[TASK B] prio=", "[TASK C] prio=" };
    static const char *const exit_msg[4] = { "", "[TASK A] exit",  "[TASK B] exit",  "[TASK C] exit" };
    u32 idx = (id < 4) ? (u32)id : 0;

    u32 slot = utsm_sched_current_slot();
    u32 prio = g_task_table[slot].priority;
    log_info(born_msg[idx]);
    log_hex64(prio_msg[idx], prio);
    drr_watchdog_register(slot, 40);   /* 400ms @100Hz */

    for (u64 iter = 1; iter <= 3; iter++) {
        drr_watchdog_kick(slot);
        log_hex64(iter_msg[idx], iter);
        utsm_sched_sleep_ms(50);
        if (g_demo_fault && id == 3) {
            /* 负向演练：停止 kick + 忙循环 → 看门狗 400ms 后捕获本任务 */
            for (;;) { __asm__ volatile("pause"); }
        }
    }
    log_info(exit_msg[idx]);
    /* entry 返回 → trampoline 自动 task_exit */
}

void utsm_sched_demo_window(void) {
    if (g_demo_ms == 0) {
        log_info("[SCHED] demo disabled (demo_ms=0)");
        return;
    }
    log_hex64("[SCHED] demo window enter, demo_ms=", g_demo_ms);

    int a = utsm_task_create(UTSM_SCHED_NO_PROCESS, 1,  demo_task_body, (void *)1ULL);
    int b = utsm_task_create(UTSM_SCHED_NO_PROCESS, 20, demo_task_body, (void *)2ULL);
    int c = utsm_task_create(UTSM_SCHED_NO_PROCESS, 40, demo_task_body, (void *)3ULL);
    log_hex64("[SCHED] demo tasks=", (u64)((a >= 0) + (b >= 0) + (c >= 0)));
    if (a < 0 || b < 0 || c < 0) {
        /* 严格错误策略插桩：demo 任务建不全 = 调度器内部状态损坏，panic（SCH-E03） */
        panic_full("SCH-E03 DEMO TASK CREATE FAILED",
                   "utsm_task_create failed inside demo window", 0);
    }

    if (utsm_sched_enable(0) != 0) {
        /* 严格错误策略插桩：enable 失败不允许静默跳过演示窗口继续启动 */
        panic_full("SCH-E02 SCHED ENABLE FAILED",
                   "utsm_sched_enable failed; skipping demo window is forbidden", 0);
        return;
    }

    /* bootstrap 忙等窗口：bootstrap 优先级最低（63），每个时间片被
     * tick 抢占后 demo 任务优先获得 CPU → 日志交错 = 抢占证据。
     * 失速兜底：若心跳长时间无进展（tick 源失效/未投递），立即中止
     * 窗口继续启动，绝不卡死 bootstrap。 */
    u64 start = drr_heartbeat_get();
    u64 ticks_needed = ((u64)g_demo_ms * 1000000ULL) / g_tick_ns + 2;
    u64 last_hb = start;
    u64 spin = 0;
    while (drr_heartbeat_get() - start < ticks_needed) {
        __asm__ volatile("pause");
        if (++spin >= 20000000ULL) {          /* ~0.1-0.5s 无进展即复查 */
            u64 now = drr_heartbeat_get();
            if (now == last_hb) {
                log_warn("[SCHED] tick stall detected, abort demo window");
                log_hex64("[SCHED]   heartbeat=", now);
                break;
            }
            last_hb = now;
            spin = 0;
        }
    }
    log_info("[SCHED] demo window done");
}

/* ===================================================================
 *  kernel_api 服务表 + selftest 钩子
 * =================================================================== */

static u64 sched_api_uptime_ns(void) { return g_uptime_ns; }

static const utsm_sched_api g_sched_api = {
    .uptime_ns = sched_api_uptime_ns,
    .yield = utsm_sched_yield,
    .current_task_slot = utsm_sched_current_slot,
    .task_create = utsm_task_create,
    .sleep_ms = utsm_sched_sleep_ms
};

const utsm_sched_api *utsm_sched_get_api(void) {
    return &g_sched_api;
}

/* ---- selftest（协作式，无 tick） ---- */

static u32 st_alloc_slot(void) {
    for (u32 i = 0; i < UTSM_SCHED_MAX_TASKS; i++) {
        if (g_task_table[i].state == UTSM_SCHED_TASK_UNUSED) return i;
    }
    return UTSM_SCHED_MAX_TASKS;
}

int utsm_sched_selftest_runqueue(void) {
    utsm_sched_tcb *t[3];
    const u32 prios[3] = { 5, 10, 63 };
    for (int k = 0; k < 3; k++) {
        u32 slot = st_alloc_slot();
        if (slot == UTSM_SCHED_MAX_TASKS) return -1;
        t[k] = &g_task_table[slot];
        if (utsm_sched_task_init(t[k], 0x9000ULL + k, UTSM_SCHED_NO_PROCESS,
                                 prios[k], ~0ULL) != UTSM_SCHED_OK) return -1;
        t[k]->flags = UTSM_SCHED_TASK_F_KERNEL;
        if (utsm_sched_enqueue(0, t[k]) != UTSM_SCHED_OK) return -1;
    }
    /* 三轮：pick 顺序必须 5,10,63（pick 即出队，不重排 → 下一优先级） */
    for (int round = 0; round < 3; round++) {
        for (int k = 0; k < 3; k++) {
            utsm_sched_tcb *p = 0;
            if (utsm_sched_pick_next(0, &p) != UTSM_SCHED_OK || p != t[k]) return -2;
            p->state = UTSM_SCHED_TASK_NEW;   /* 模拟"不在队列" */
        }
        for (int k = 0; k < 3; k++) {
            if (utsm_sched_enqueue(0, t[k]) != UTSM_SCHED_OK) return -3;
        }
    }
    for (int k = 0; k < 3; k++) {
        utsm_sched_dequeue(t[k]);
        t[k]->state = UTSM_SCHED_TASK_UNUSED;
    }
    return 0;
}

int utsm_sched_selftest_blockwake(void) {
    u32 sa = st_alloc_slot();
    if (sa == UTSM_SCHED_MAX_TASKS) return -1;
    utsm_sched_tcb *a = &g_task_table[sa];
    if (utsm_sched_task_init(a, 0xA001, UTSM_SCHED_NO_PROCESS, 20, ~0ULL) != UTSM_SCHED_OK) return -1;
    u32 sb = st_alloc_slot();   /* a 已 init（NEW），不会重复分配同一 slot */
    if (sb == UTSM_SCHED_MAX_TASKS) return -1;
    utsm_sched_tcb *b = &g_task_table[sb];
    if (utsm_sched_task_init(b, 0xA002, UTSM_SCHED_NO_PROCESS, 30, ~0ULL) != UTSM_SCHED_OK) return -1;
    utsm_sched_enqueue(0, a);
    utsm_sched_enqueue(0, b);

    /* block a → pick 跳过 a 取 b */
    utsm_sched_block(a, UTSM_SCHED_TASK_BLOCKED);
    utsm_sched_tcb *p = 0;
    if (utsm_sched_pick_next(0, &p) != UTSM_SCHED_OK || p != b) return -2;
    p->state = UTSM_SCHED_TASK_NEW;

    /* wake a → 回到队列 → pick 取 a */
    utsm_sched_wake(a);
    if (utsm_sched_pick_next(0, &p) != UTSM_SCHED_OK || p != a) return -3;
    p->state = UTSM_SCHED_TASK_NEW;

    utsm_sched_dequeue(a);
    utsm_sched_dequeue(b);
    a->state = UTSM_SCHED_TASK_UNUSED;
    b->state = UTSM_SCHED_TASK_UNUSED;
    return 0;
}

int utsm_sched_debug_ready_count(u32 cpu_id) {
    if (cpu_id >= UTSM_SCHED_MAX_CPUS) return -1;
    return (int)g_rq_ptrs[cpu_id]->ready_count;
}
