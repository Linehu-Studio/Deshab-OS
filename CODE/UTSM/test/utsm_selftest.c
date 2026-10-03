#include <utsm/utsm.h>
#include <utsm/pckc.h>
#include <utsm/crypto.h>
#include <utsm/dma.h>
#include <utsm/dkm.h>
#include <utsm/log.h>
#include <utsm/segment.h>
#include <utsm/arena.h>
#include <utsm/drr.h>
#include <utsm/sched_ext.h>
#include <utsm/panic.h>

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
        panic_full("TST-E01 SELFTEST CTX ALLOC FAILED",
                   "utsm_selftest: process context allocation failed", 0);
        return UTSM_ERR_NO_MEMORY;
    }
    utsm_process_create_test(process, 1);

    utsm_capability cap;
    int status = utsm_create_segment(process, UTSM_PAGE_SIZE, UTSM_SEG_F_READ | UTSM_SEG_F_WRITE | UTSM_SEG_F_STRONG_RECOVERY, &cap);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest create segment failed");
        panic_full("TST-E02 SELFTEST CREATE SEGMENT FAILED",
                   "utsm_selftest: create segment failed", 0);
        return status;
    }

    const char *message = "deshab utsm sealed memory selftest";
    u64 len = strlen(message) + 1;
    status = utsm_write(cap, 128, message, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest write failed");
        panic_full("TST-E03 SELFTEST WRITE FAILED",
                   "utsm_selftest: sealed write failed", 0);
        return status;
    }
    log_info("[UTSM] selftest write ok");

    utsm_segment_desc *desc = utsm_get_segment(cap.segment_slot);
    if (!desc) {
        panic_full("TST-E04 SELFTEST SEG DESC MISSING",
                   "utsm_selftest: segment desc lookup failed", 0);
        return UTSM_ERR_INVALID;
    }
    if (memcmp(desc->cipher_base + 128, message, len) == 0) {
        log_error("[UTSM] raw cipher equals plaintext");
        panic_full("TST-E05 SELFTEST CIPHER PLAINTEXT LEAK",
                   "utsm_selftest: raw cipher equals plaintext", 0);
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] raw cipher check ok");

    char readback[64];
    status = utsm_read(cap, 128, readback, len);
    if (status != UTSM_OK) {
        log_error("[UTSM] selftest read failed");
        panic_full("TST-E06 SELFTEST READ FAILED",
                   "utsm_selftest: sealed read failed", 0);
        return status;
    }
    if (memcmp(readback, message, len) != 0) {
        log_error("[UTSM] selftest read mismatch");
        panic_full("TST-E07 SELFTEST READ MISMATCH",
                   "utsm_selftest: readback mismatch", 0);
        return UTSM_ERR_INVALID;
    }
    log_info("[UTSM] selftest read ok");

    /* ==== U5: chacha20 KAT（RFC 8439 §2.4.2 官方向量）====
     * 历史注：FUCK 注释曾提"并行会话 chacha20 KAT 间歇 FAIL"——该测试
     * 从未合并进本树（幽灵缺陷）。本 KAT 为确定性官方向量，不依赖
     * 硬件/时钟/随机数，PASS/FAIL 完全可复现。期望值由
     * test/gen_chacha_kat.py 生成，并经 pycryptodome 独立对拍。 */
    {
#include "chacha_kat.inc"
        u8 ks[64];
        utsm_chacha20_block(kat_key, kat_nonce, kat_counter, ks);
        if (memcmp(ks, kat_expect, 64) != 0) {
            log_error("[UTSM] selftest chacha20 KAT FAIL (RFC 8439 §2.4.2)");
            panic_full("TST-E08 CHACHA20 KAT FAIL",
                       "utsm_selftest: chacha20 KAT mismatch (RFC 8439 2.4.2)", 0);
            return UTSM_ERR_INVALID;
        }
        log_info("[UTSM] selftest chacha20 KAT ok (RFC 8439 §2.4.2)");

        /* page MAC 确定性：同输入同 MAC、跨页不同 MAC */
        const u64 *rk = drr_get_root_key();
        u64 m1 = utsm_page_mac(rk, 0, 0, 0, desc->cipher_base);
        u64 m2 = utsm_page_mac(rk, 0, 0, 0, desc->cipher_base);
        u64 m3 = utsm_page_mac(rk, 0, 1, 0, desc->cipher_base);
        if (m1 != m2 || m1 == m3) {
            log_error("[UTSM] selftest page MAC determinism FAIL");
            panic_full("TST-E09 PAGE MAC DETERMINISM FAIL",
                       "utsm_selftest: page MAC determinism broken", 0);
            return UTSM_ERR_INVALID;
        }
        log_info("[UTSM] selftest page MAC determinism ok");

        /* ==== U3+: 封缄内存 .sealed 区 MAC 校验（UTSM-E03 路径自证） ==== */
        {
            extern void utsm_sealed_verify(void);
            utsm_sealed_verify();   /* 不符即 panic，返回即通过 */
            log_info("[UTSM] selftest sealed MAC verify ok");
        }
    }

    /* ==== U4: 密文页 dump —— 串口输出密文/明文 hex 对照 ==== */
    {
        int dst = utrw_debug_dump_page(cap.segment_slot, 0, 128);
        if (dst != UTSM_OK) {
            log_error("[UTSM] selftest cipher dump failed");
            panic_full("TST-E10 SELFTEST CIPHER DUMP FAILED",
                       "utsm_selftest: cipher page dump failed", 0);
            return dst;
        }
    }

    /* ==== U2: Slow Path 真分支（EPOCH 刷新 / KEY_MISS 重派生 / POISONED 拒绝） ==== */
    {
        /* 1) EPOCH 过期：伪造 stale cap（epoch=desc->key_epoch+1），slow path 应
         *    返回 OK 并回传刷新后的 cap，重试 read 应成功且内容一致 */
        utsm_capability stale = cap;
        stale.epoch = (u32)desc->key_epoch + 1;
        utsm_capability refreshed = stale;
        status = utsm_slow_path(stale, UTSM_RIGHT_READ, 128, len,
                                UTSM_ERR_EPOCH, &refreshed);
        if (status != UTSM_OK) {
            log_error("[UTSM] selftest slow EPOCH recover failed");
            panic_full("TST-E11 SLOW EPOCH RECOVER FAILED",
                       "utsm_selftest: slow path EPOCH recovery failed", 0);
            return status;
        }
        char slow_rb[64];
        status = utsm_read(refreshed, 128, slow_rb, len);
        if (status != UTSM_OK || memcmp(slow_rb, message, len) != 0) {
            log_error("[UTSM] selftest slow EPOCH retry read failed");
            panic_full("TST-E12 SLOW EPOCH RETRY FAILED",
                       "utsm_selftest: read after EPOCH refresh failed", 0);
            return (status == UTSM_OK) ? UTSM_ERR_INVALID : status;
        }
        log_info("[UTSM] selftest slow EPOCH ok");

        /* 2) KEY_MISS：清掉该段 PCKC 缓存，slow path 重派生应返回 OK */
        utsm_pckc_invalidate_slot(cap.segment_slot);
        status = utsm_slow_path(refreshed, UTSM_RIGHT_READ, 128, len,
                                UTSM_ERR_KEY_MISS, 0);
        if (status != UTSM_OK) {
            log_error("[UTSM] selftest slow KEY_MISS recover failed");
            panic_full("TST-E13 SLOW KEY_MISS RECOVER FAILED",
                       "utsm_selftest: slow path KEY_MISS re-derive failed", 0);
            return status;
        }
        status = utsm_read(refreshed, 128, slow_rb, len);
        if (status != UTSM_OK || memcmp(slow_rb, message, len) != 0) {
            log_error("[UTSM] selftest slow KEY_MISS retry read failed");
            panic_full("TST-E14 SLOW KEY_MISS RETRY FAILED",
                       "utsm_selftest: read after KEY_MISS re-derive failed", 0);
            return (status == UTSM_OK) ? UTSM_ERR_INVALID : status;
        }
        log_info("[UTSM] selftest slow KEY_MISS ok");

        /* 3) POISONED：不可恢复，原样返回；DRR fault 留痕不重启
         *    （selftest 场景 drr_report_fault 只计数，无 reboot 语义） */
        status = utsm_slow_path(refreshed, UTSM_RIGHT_READ, 128, len,
                                UTSM_ERR_POISONED, 0);
        if (status != UTSM_ERR_POISONED) {
            log_error("[UTSM] selftest slow POISONED passthrough failed");
            panic_full("TST-E15 SLOW POISONED PASSTHROUGH FAILED",
                       "utsm_selftest: POISONED not passed through", 0);
            return UTSM_ERR_INVALID;
        }
        log_info("[UTSM] selftest slow POISONED ok");
    }

    /* ==== U3: @sealed 可选标记 —— unsealed 段明文直存（0 加解密开销） ==== */
    {
        utsm_capability ucap;
        status = utsm_create_segment(process, UTSM_PAGE_SIZE,
                                     UTSM_SEG_F_READ | UTSM_SEG_F_WRITE |
                                         UTSM_SEG_F_UNSEALED,
                                     &ucap);
        if (status != UTSM_OK) {
            log_error("[UTSM] selftest unsealed segment create failed");
            panic_full("TST-E16 UNSEALED SEGMENT CREATE FAILED",
                       "utsm_selftest: unsealed segment create failed", 0);
            return status;
        }
        const char *upl = "unsealed plain text 00";
        u64 ulen = strlen(upl) + 1;
        status = utsm_write(ucap, 128, upl, ulen);
        if (status != UTSM_OK) {
            log_error("[UTSM] selftest unsealed write failed");
            panic_full("TST-E17 UNSEALED WRITE FAILED",
                       "utsm_selftest: unsealed plain write failed", 0);
            return status;
        }
        utsm_segment_desc *udesc = utsm_get_segment(ucap.segment_slot);
        if (!udesc) {
            panic_full("TST-E18 UNSEALED DESC MISSING",
                       "utsm_selftest: unsealed segment desc lookup failed", 0);
            return UTSM_ERR_INVALID;
        }
        /* 明文直存：存储区内容必须与明文完全一致（0 开销的可观察证据） */
        if (memcmp(udesc->cipher_base + 128, upl, ulen) != 0) {
            log_error("[UTSM] selftest unsealed store not plaintext");
            panic_full("TST-E19 UNSEALED STORE NOT PLAINTEXT",
                       "utsm_selftest: unsealed storage differs from plaintext", 0);
            return UTSM_ERR_INVALID;
        }
        char urb[64];
        status = utsm_read(ucap, 128, urb, ulen);
        if (status != UTSM_OK || memcmp(urb, upl, ulen) != 0) {
            log_error("[UTSM] selftest unsealed read failed");
            panic_full("TST-E20 UNSEALED READ FAILED",
                       "utsm_selftest: unsealed readback mismatch", 0);
            return (status == UTSM_OK) ? UTSM_ERR_INVALID : status;
        }
        log_info("[UTSM] selftest unsealed segment ok (plain storage)");
    }

    /* ==== D3: 按名导出表（kapi）——按名取服务并调用 ==== */
    {
        const dkm_kernel_api *api = dkm_get_kernel_api();
        if (!api->kapi || api->kapi->magic != DKM_KAPI_MAGIC) {
            log_error("[UTSM] selftest kapi table missing");
            panic_full("TST-E21 KAPI TABLE MISSING",
                       "utsm_selftest: kapi export table missing or bad magic", 0);
            return UTSM_ERR_INVALID;
        }
        void *fn = 0;
        if (api->kapi->lookup("log.info", &fn) != 0) {
            log_error("[UTSM] selftest kapi lookup log.info failed");
            panic_full("TST-E22 KAPI LOOKUP LOG FAILED",
                       "utsm_selftest: kapi lookup log.info failed", 0);
            return UTSM_ERR_INVALID;
        }
        void (*klog)(const char *) = (void (*)(const char *))fn;
        klog("[UTSM] selftest kapi: log.info via export table");
        if (api->kapi->lookup("dma.alloc_pages", &fn) != 0) {
            log_error("[UTSM] selftest kapi lookup dma failed");
            panic_full("TST-E23 KAPI LOOKUP DMA FAILED",
                       "utsm_selftest: kapi lookup dma.alloc_pages failed", 0);
            return UTSM_ERR_INVALID;
        }
        int (*kalloc)(u64, u64, u64, dkm_dma_buffer *) = (int (*)(u64, u64, u64, dkm_dma_buffer *))fn;
        dkm_dma_buffer kbuf;
        if (kalloc(1, 4096, 0xFFFFFFFFULL, &kbuf) != 0 || !kbuf.virt) {
            log_error("[UTSM] selftest kapi dma alloc failed");
            panic_full("TST-E24 KAPI DMA ALLOC FAILED",
                       "utsm_selftest: kapi dma.alloc_pages call failed", 0);
            return UTSM_ERR_INVALID;
        }
        if (api->kapi->lookup("no.such.service", &fn) == 0) {
            log_error("[UTSM] selftest kapi negative lookup failed");
            panic_full("TST-E25 KAPI NEGATIVE LOOKUP FAILED",
                       "utsm_selftest: negative lookup unexpectedly succeeded", 0);
            return UTSM_ERR_INVALID;
        }
        log_hex64("[UTSM] selftest kapi entries=", api->kapi->count);
        log_info("[UTSM] selftest kapi ok");
    }

    /* ==== SCHED-1: O(1) runqueue 语义（合成 TCB，协作式，无 tick） ==== */
    /* ==== SCHED-1 + SCHED-2: runqueue / block-wake（tick 竞态保护） ==== */
    {
        u64 st_flags = selftest_irq_save();
        int rq_rc = utsm_sched_selftest_runqueue();
        int bw_rc = (rq_rc == 0) ? utsm_sched_selftest_blockwake() : 0;
        selftest_irq_restore(st_flags);
        if (rq_rc != 0) {
            log_error("[SCHED] selftest: runqueue O(1) FAIL");
            panic_full("TST-E26 RUNQUEUE SELFTEST FAIL",
                       "utsm_selftest: O(1) runqueue semantics broken", 0);
            return UTSM_ERR_INVALID;
        }
        log_info("[SCHED] selftest: runqueue O(1) PASS");
        if (bw_rc != 0) {
            log_error("[SCHED] selftest: block/wake FAIL");
            panic_full("TST-E27 BLOCKWAKE SELFTEST FAIL",
                       "utsm_selftest: block/wake semantics broken", 0);
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
            panic_full("TST-E28 DRR P1 WRITE FAILED",
                       "utsm_selftest: DRR pre-snapshot write failed", 0);
            return status;
        }
        int snap = drr_ckpt_snapshot_dirty();
        log_hex64("[DRR] ckpt snapshot pages=", (u64)(i64)snap);
        if (snap < 1) {
            log_error("[DRR] selftest snapshot failed");
            panic_full("TST-E29 DRR SNAPSHOT FAILED",
                       "utsm_selftest: dirty page snapshot failed", 0);
            return UTSM_ERR_INVALID;
        }
        drr_checkpoint_meta meta;
        if (drr_checkpoint_recover(&meta) != 0 || meta.type != DRR_CKPT_DIRTY_PAGE ||
            meta.dirty_page_count < 1) {
            log_error("[DRR] selftest meta verify failed");
            panic_full("TST-E30 DRR META VERIFY FAILED",
                       "utsm_selftest: checkpoint meta verify failed", 0);
            return UTSM_ERR_INVALID;
        }
        status = utsm_write(cap, 512, p2, plen);
        if (status != UTSM_OK) {
            log_error("[DRR] selftest P2 write failed");
            panic_full("TST-E31 DRR P2 WRITE FAILED",
                       "utsm_selftest: DRR overwrite write failed", 0);
            return status;
        }
        int rb = drr_rollback_pages();
        log_hex64("[DRR] rollback pages=", (u64)(i64)rb);
        if (rb < 1) {
            log_error("[DRR] selftest rollback failed");
            panic_full("TST-E32 DRR ROLLBACK FAILED",
                       "utsm_selftest: page rollback failed", 0);
            return UTSM_ERR_INVALID;
        }
        status = utsm_read(cap, 512, readback, plen);
        if (status != UTSM_OK || memcmp(readback, p1, plen) != 0) {
            log_error("[DRR] rollback verify FAIL");
            panic_full("TST-E33 DRR ROLLBACK VERIFY FAIL",
                       "utsm_selftest: rollback verify mismatch", 0);
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
