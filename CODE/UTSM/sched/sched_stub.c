/*
 * Isolated scheduler placeholder.
 *
 * Deliberately not linked by the UTSM build. These functions make accidental
 * use fail explicitly until the per-CPU bitmap scheduler and switch path are
 * implemented.
 */

#include <utsm/sched.h>

int utsm_sched_init(void) {
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_cpu_init(u32 cpu_id, utsm_sched_run_queue *queue) {
    UTSM_UNUSED(cpu_id);
    UTSM_UNUSED(queue);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_task_init(utsm_sched_tcb *task, u64 task_id,
                         u32 process_slot, u32 priority, u64 affinity_mask) {
    UTSM_UNUSED(task);
    UTSM_UNUSED(task_id);
    UTSM_UNUSED(process_slot);
    UTSM_UNUSED(priority);
    UTSM_UNUSED(affinity_mask);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_enqueue(u32 cpu_id, utsm_sched_tcb *task) {
    UTSM_UNUSED(cpu_id);
    UTSM_UNUSED(task);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_dequeue(utsm_sched_tcb *task) {
    UTSM_UNUSED(task);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_block(utsm_sched_tcb *task, u32 blocked_state) {
    UTSM_UNUSED(task);
    UTSM_UNUSED(blocked_state);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_wake(utsm_sched_tcb *task) {
    UTSM_UNUSED(task);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_pick_next(u32 cpu_id, utsm_sched_tcb **out_task) {
    UTSM_UNUSED(cpu_id);
    UTSM_UNUSED(out_task);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}

int utsm_sched_tick(u32 cpu_id, u64 elapsed_ns,
                    utsm_sched_tcb **out_preempted_task) {
    UTSM_UNUSED(cpu_id);
    UTSM_UNUSED(elapsed_ns);
    UTSM_UNUSED(out_preempted_task);
    return UTSM_SCHED_ERR_UNSUPPORTED;
}
