#ifndef UTSM_SCHED_H
#define UTSM_SCHED_H

#include <utsm/types.h>

/*
 * Experimental SAS-R0-PCQ scheduler contract.
 *
 * This header reserves the first ABI shape for a future per-CPU O(1)
 * priority-bitmap scheduler. It is not connected to boot or context-switch
 * code. Structures begin with version/size where they may cross component
 * boundaries; existing fields must not be reordered and extensions append.
 */

#define UTSM_SCHED_ABI_VERSION       1u
#define UTSM_SCHED_PRIORITY_COUNT   64u
#define UTSM_SCHED_INVALID_SLOT     0xffffffffu
#define UTSM_SCHED_PRIORITY_HIGHEST 0u
#define UTSM_SCHED_PRIORITY_LOWEST  (UTSM_SCHED_PRIORITY_COUNT - 1u)

/* Explicit API results. The isolated stub returns UNSUPPORTED for every call. */
typedef enum utsm_sched_result {
    UTSM_SCHED_OK              = 0,
    UTSM_SCHED_ERR_INVALID     = -1,
    UTSM_SCHED_ERR_STATE       = -2,
    UTSM_SCHED_ERR_NO_TASK     = -3,
    UTSM_SCHED_ERR_UNSUPPORTED = -38
} utsm_sched_result;

/* Values are stored in u32 fields so the TCB layout does not depend on enum size. */
typedef enum utsm_sched_task_state {
    UTSM_SCHED_TASK_UNUSED = 0,
    UTSM_SCHED_TASK_NEW,
    UTSM_SCHED_TASK_READY,
    UTSM_SCHED_TASK_RUNNING,
    UTSM_SCHED_TASK_BLOCKED,
    UTSM_SCHED_TASK_SLEEPING,
    UTSM_SCHED_TASK_RECOVERING,
    UTSM_SCHED_TASK_FAULTED,
    UTSM_SCHED_TASK_ZOMBIE
} utsm_sched_task_state;

#define UTSM_SCHED_TASK_F_KERNEL      (1u << 0)
#define UTSM_SCHED_TASK_F_IDLE        (1u << 1)
#define UTSM_SCHED_TASK_F_NO_MIGRATE  (1u << 2)
#define UTSM_SCHED_TASK_F_RECOVERY    (1u << 3)

/*
 * Minimum x86_64 resume context. Volatile registers belong to the eventual
 * interrupt/syscall frame and are deliberately not duplicated here.
 */
typedef struct utsm_sched_arch_context {
    u64 rsp;
    u64 rip;
    u64 rflags;
    u64 rbx;
    u64 rbp;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
} utsm_sched_arch_context;

/*
 * Task control block prefix.
 *
 * runq_prev/runq_next are task-table slots, not pointers, so queues remain
 * relocatable. priority 0 is highest. crypto_context is loaded as a pointer
 * during a future switch; switch code must not scan capabilities or perform
 * cryptography/checkpoint work.
 */
typedef struct utsm_sched_tcb {
    u32 abi_version;
    u32 struct_size;

    u64 task_id;
    u32 process_slot;
    u32 flags;

    u32 state;              /* utsm_sched_task_state */
    u32 priority;
    u32 assigned_cpu;
    u32 last_cpu;
    u64 affinity_mask;

    u64 timeslice_ns;
    u64 wake_deadline_ns;
    void *crypto_context;

    u32 runq_prev;
    u32 runq_next;
    utsm_sched_arch_context arch;

    u64 reserved[4];
} utsm_sched_tcb;

/*
 * One ready bitmap and one intrusive FIFO per priority form the planned O(1)
 * run queue. A set bit means that head[priority] is valid. Synchronization is
 * an implementation concern and is intentionally not implied by this layout.
 */
typedef struct utsm_sched_run_queue {
    u32 abi_version;
    u32 struct_size;
    u32 cpu_id;
    u32 flags;

    u64 ready_bitmap;
    u32 head[UTSM_SCHED_PRIORITY_COUNT];
    u32 tail[UTSM_SCHED_PRIORITY_COUNT];

    u32 current_task_slot;
    u32 idle_task_slot;
    u32 ready_count;
    u32 reserved0;
} utsm_sched_run_queue;

int utsm_sched_init(void);
int utsm_sched_cpu_init(u32 cpu_id, utsm_sched_run_queue *queue);
int utsm_sched_task_init(utsm_sched_tcb *task, u64 task_id,
                         u32 process_slot, u32 priority, u64 affinity_mask);
int utsm_sched_enqueue(u32 cpu_id, utsm_sched_tcb *task);
int utsm_sched_dequeue(utsm_sched_tcb *task);
int utsm_sched_block(utsm_sched_tcb *task, u32 blocked_state);
int utsm_sched_wake(utsm_sched_tcb *task);
int utsm_sched_pick_next(u32 cpu_id, utsm_sched_tcb **out_task);
int utsm_sched_tick(u32 cpu_id, u64 elapsed_ns,
                    utsm_sched_tcb **out_preempted_task);

#endif /* UTSM_SCHED_H */
