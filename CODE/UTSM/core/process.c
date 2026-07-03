/* process.c — UTSM 进程上下文管理
 *
 * 进程表分配/释放、capability table 操作、crypto context 管理。
 */

#include <utsm/process.h>
#include <utsm/drr.h>
#include <utsm/arena.h>
#include <utsm/log.h>

/* ---- 进程表 ---- */
static utsm_process_context g_process_table[UTSM_MAX_PROCESSES];
static u32 g_process_count;

void utsm_process_init(void) {
    log_info("[UTSM] process table init");
    for (u32 i = 0; i < UTSM_MAX_PROCESSES; i++) {
        g_process_table[i].state = UTSM_PROC_UNUSED;
        g_process_table[i].process_slot = i;
        g_process_table[i].cap_table.count = 0;
        g_process_table[i].cap_table.generation = 0;
    }
    g_process_count = 0;
}

void utsm_process_create_test(utsm_process_context *process, u64 seed) {
    process->process_uuid = uuid_make_test(seed);
    process->crypto_epoch = 1;
    process->recovery_generation = drr_recovery_generation();
    process->capability_count = 0;
    process->flags = UTSM_PROC_F_KERNEL;
    process->state = UTSM_PROC_ACTIVE;
    process->hot_segment_slot = 0;
    process->drr_recovery_gen = drr_recovery_generation();

    /* 初始化 crypto context */
    process->crypto_ctx.crypto_epoch = 1;
    process->crypto_ctx.key_derivation_seed = seed ^ 0x9e3779b97f4a7c15ULL;
    process->crypto_ctx.pckc_hot_hint = 0;

    /* 初始化 capability table */
    process->cap_table.count = 0;
    process->cap_table.generation = 1;
}

utsm_process_context *utsm_process_alloc(void) {
    for (u32 i = 0; i < UTSM_MAX_PROCESSES; i++) {
        if (g_process_table[i].state == UTSM_PROC_UNUSED) {
            utsm_process_context *p = &g_process_table[i];
            p->process_slot = i;
            p->state = UTSM_PROC_ACTIVE;
            p->crypto_epoch = 1;
            p->recovery_generation = drr_recovery_generation();
            p->capability_count = 0;
            p->flags = 0;
            p->hot_segment_slot = 0;
            p->drr_recovery_gen = drr_recovery_generation();
            p->cap_table.count = 0;
            p->cap_table.generation = 1;
            p->crypto_ctx.crypto_epoch = 1;
            p->crypto_ctx.key_derivation_seed = 0;
            p->crypto_ctx.pckc_hot_hint = 0;
            g_process_count++;
            return p;
        }
    }
    return NULL;
}

void utsm_process_free(utsm_process_context *process) {
    if (!process) return;
    process->state = UTSM_PROC_UNUSED;
    process->cap_table.count = 0;
    process->capability_count = 0;
    if (g_process_count > 0) g_process_count--;
}

utsm_process_context *utsm_process_get(u32 slot) {
    if (slot >= UTSM_MAX_PROCESSES) return NULL;
    return &g_process_table[slot];
}

/* ---- Capability 操作 ---- */
int utsm_process_grant_cap(utsm_process_context *process, utsm_capability cap) {
    if (!process || process->cap_table.count >= UTSM_MAX_CAPABILITIES) {
        return -1;
    }
    u32 idx = process->cap_table.count;
    process->cap_table.caps[idx] = cap;
    process->cap_table.count++;
    process->capability_count = process->cap_table.count;
    process->cap_table.generation++;
    return 0;
}

int utsm_process_revoke_cap(utsm_process_context *process, u32 cap_index) {
    if (!process || cap_index >= process->cap_table.count) {
        return -1;
    }
    /* 将最后一个 capability 移到被删除的位置 */
    u32 last = process->cap_table.count - 1;
    if (cap_index != last) {
        process->cap_table.caps[cap_index] = process->cap_table.caps[last];
    }
    process->cap_table.count--;
    process->capability_count = process->cap_table.count;
    process->cap_table.generation++;
    return 0;
}

utsm_capability *utsm_process_get_cap(utsm_process_context *process, u32 cap_index) {
    if (!process || cap_index >= process->cap_table.count) {
        return NULL;
    }
    return &process->cap_table.caps[cap_index];
}

/* ---- Crypto context 操作 ---- */
void utsm_process_advance_epoch(utsm_process_context *process) {
    if (!process) return;
    process->crypto_epoch++;
    process->crypto_ctx.crypto_epoch = process->crypto_epoch;
    /* 清空 PCKC 热提示（epoch 变化后旧缓存无效） */
    process->crypto_ctx.pckc_hot_hint = 0;
    process->hot_segment_slot = 0;
}
