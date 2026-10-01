#include <utsm/utsm.h>
#include <utsm/log.h>
#include <utsm/segment.h>
#include <utsm/arena.h>
#include <utsm/drr.h>
#include <utsm/sched_ext.h>

int memcmp(const void *a, const void *b, usize len);
usize strlen(const char *s);

/* selftest 与 LAPIC/PIT tick 竞态：tick 在别的任务上唤醒 sleeper 时
 * 会向同一 runqueue 入队，破坏 pick_next 的确定性断言（偶发
 * block/wake FAIL → arch_halt_forever）。临界区内关中断。 */
static u64 selftest_irq_save(void) {
    u64 flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void selftest_irq_restore(u64 flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory");
}

int utsm_selftest_run(void) {
    log_info("[UTSM] selftest enter");

    /* utsm_process_context is ~8KB (256 capabilities × 32 bytes).
     * Allocate from arena instead of stack to avoid kernel stack overflow. */
    utsm_process_context *process = (utsm_process_context *)kmem_alloc(sizeof(utsm_process_context));
    if (!process) {
        log_error("[UTSM] selftest: failed to allocate process context");
        return UTSM_ERR_NO_MEMORY;
    }
    utsm_process_create_test(process, 1);

    utsm_capability cap;
    int status = utsm_create_segment(process, UTSM_PAGE_SIZE, UTSM_SEG_F_READ | UTSM_SEG_F_WRITE | UTSM_SEG_F_STRONG_RECOVERY, &cap);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest create segment failed");
        return status;
    }

    const char *message = "deshab utsm sealed memory selftest";
    u64 len = strlen(message) + 1;
    status = utsm_write(cap, 128, message, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest write failed");
        return status;
    }
    log_info("[UTSM] selftest write ok");

    utsm_segment_desc *desc = utsm_get_segment(cap.segment_slot);
    if (!desc) {
        return UTSM_ERR_INVALID;
    }
    if (memcmp(desc->cipher_base + 128, message, len) == 0) {
        log_error("[UTSM] raw cipher equals plaintext");
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] raw cipher check ok");

    char readback[64];
    status = utsm_read(cap, 128, readback, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest read failed");
        return status;
    }
    if (memcmp(readback, message, len) != 0) {
        log_error("[UTSM] selftest read mismatch");
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] selftest read ok");

    /* ==== SCHED-1: O(1) runqueue 语义（合成 TCB，协作式，无 tick） ==== */
    /* ==== SCHED-1 + SCHED-2: runqueue / block-wake（tick 竞态保护） ==== */
    {
        u64 st_flags = selftest_irq_save();
        int rq_rc = utsm_sched_selftest_runqueue();
        int bw_rc = (rq_rc == 0) ? utsm_sched_selftest_blockwake() : 0;
        selftest_irq_restore(st_flags);
        if (rq_rc != 0) {
            log_error("[SCHED] selftest: runqueue O(1) FAIL");
            return UTSM_ERR_INVALID;
        }
        log_info("[SCHED] selftest: runqueue O(1) PASS");
        if (bw_rc != 0) {
            log_error("[SCHED] selftest: block/wake FAIL");
            return UTSM_ERR_INVALID;
        }
        log_info("[SCHED] selftest: block/wake PASS");
    }

    /* ==== DRR-1: 真实脏页快照 + 页级回滚 ====
     * 写 P1 → 快照（捕获 P1 密文页）→ 写 P2 覆盖 → 回滚 → 读回 P1 */
    {
        const char *p1 = "P1-ROLLBACK-STATE-01";
        const char *p2 = "P2-OVERWRITE-STATE-0";
        u64 plen = strlen(p1) + 1;
        status = utsm_write(cap, 512, p1, plen);
        if (status != UTSM_OK) {
            log_error("[DRR] selftest P1 write failed");
            return status;
        }
        int snap = drr_ckpt_snapshot_dirty();
        log_hex64("[DRR] ckpt snapshot pages=", (u64)(i64)snap);
        if (snap < 1) {
            log_error("[DRR] selftest snapshot failed");
            return UTSM_ERR_INVALID;
        }
        drr_checkpoint_meta meta;
        if (drr_checkpoint_recover(&meta) != 0 || meta.type != DRR_CKPT_DIRTY_PAGE ||
            meta.dirty_page_count < 1) {
            log_error("[DRR] selftest meta verify failed");
            return UTSM_ERR_INVALID;
        }
        status = utsm_write(cap, 512, p2, plen);
        if (status != UTSM_OK) {
            log_error("[DRR] selftest P2 write failed");
            return status;
        }
        int rb = drr_rollback_pages();
        log_hex64("[DRR] rollback pages=", (u64)(i64)rb);
        if (rb < 1) {
            log_error("[DRR] selftest rollback failed");
            return UTSM_ERR_INVALID;
        }
        status = utsm_read(cap, 512, readback, plen);
        if (status != UTSM_OK || memcmp(readback, p1, plen) != 0) {
            log_error("[DRR] rollback verify FAIL");
            return UTSM_ERR_INVALID;
        }
        log_info("[DRR] rollback verify ok");
    }

    /* ==== DRR-2: 看门狗判定（dry-run：reboot 关闭，仅验证日志路径） ==== */
    {
        drr_set_reboot_enabled(0);
        drr_watchdog_register(0xFFFFFEu, 2);
        drr_heartbeat_tick();
        drr_heartbeat_tick();
        drr_heartbeat_tick();       /* 3 ticks > timeout 2 → 触发 */
        drr_watchdog_check();       /* 期望 [DRR] FAULT: watchdog timeout */
        drr_watchdog_unregister(0xFFFFFEu);
        drr_set_reboot_enabled(1);
        log_info("[DRR] watchdog selftest done");
    }

    return UTSM_OK;
}
