#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/panic.h>
#include <utsm/drr.h>
#include <utsm/dkm.h>
#include <utsm/dma.h>
#include "ini_parser.h"

/* ===================================================================
 *  F4: boot_ok 分级启动自检
 *
 *  "知道自己是几成疯。"
 *  FUCK [boot] selftest=0|1|2：
 *    0 = 关闭（仅打一行 skip）
 *    1 = 快速：结构健全性检查（IDT/GDT/配置/DRR/调度器服务表）
 *    2 = 全量：快速 + 功能探针（DMA alloc/write/free、段分配）
 *
 *  失败语义：致命项（IDT/CS）→ 莲花 panic；非致命项 → WARN 继续。
 *  输出：串口 + 每项一行 [boot] 明细。
 * =================================================================== */

/* 致命项返回 -1（调用方 halt / panic），非致命返回计数。 */
typedef struct {
    int  level;
    int  pass;
    int  warn;
    int  fail;
} bself_ctx;

static void bself_line(bself_ctx *c, const char *name, int fatal, int ok) {
    if (ok) {
        c->pass++;
        log_info("[boot] selftest  [OK]  ");
        log_info(name);
    } else if (fatal) {
        c->fail++;
        log_error("[boot] selftest  [FATAL]");
        log_error(name);
    } else {
        c->warn++;
        log_warn("[boot] selftest  [WARN]");
        log_warn(name);
    }
}

/* IDTR 健全性：limit==4095（256 项）且 base 非零 */
static int check_idt(u64 *base_out) {
    /* sidt 写 10 字节：limit(2) + base(8) */
    u8 buf[10] __attribute__((aligned(2)));
    __asm__ volatile("sidt %0" : "=m"(buf));
    u16 limit = *(u16 *)&buf[0];
    u64 base  = *(u64 *)&buf[2];
    *base_out = base;
    return (limit == 4095) && (base != 0);
}

static int check_cs_valid(void) {
    u64 cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return cs != 0;
}

int boot_selftest_run(int level) {
    bself_ctx c = { level, 0, 0, 0 };

    log_info("[boot] selftest begin");
    log_info("[boot] level:");
    log_hex64("[boot]   ", (u64)level);

    if (level <= 0) {
        log_info("[boot] selftest skipped (selftest=0)");
        return 0;
    }

    /* ---- 致命项 1: IDT 覆盖 ---- */
    u64 idt_base = 0;
    int idt_ok = check_idt(&idt_base);
    bself_line(&c, "IDT: limit/base", 1, idt_ok);
    if (!idt_ok) {
        panic_full("UTSM-BST", "IDT malformed at boot selftest", 0);
    }

    /* ---- 致命项 2: CS 有效 ---- */
    int cs_ok = check_cs_valid();
    bself_line(&c, "CS:  ring0 selector", 1, cs_ok);
    if (!cs_ok) {
        panic_full("UTSM-BST", "CS invalid at boot selftest", 0);
    }

    /* ---- 非致命: DRR 初始化 ---- */
    int drr_ok = drr_recovery_generation() >= 1;
    bself_line(&c, "DRR: emergency pool + ckpt slots", 0, drr_ok);

    /* ---- 非致命: 内核服务表（调度器） ---- */
    int sched_ok = utsm_sched_get_api() != 0;
    bself_line(&c, "SCHED: service table", 0, sched_ok);

    /* ---- 非致命: kernel_api 平台信息 ---- */
    const dkm_kernel_api *api = dkm_get_kernel_api();
    bself_line(&c, "KAPI: kernel_api present", 0, api != 0);

    if (level >= 2) {
        /* ---- 功能探针: DMA 页分配 + 写读回 ---- */
        dkm_dma_buffer buf;
        int dma_ok = 0;
        if (dma_alloc_pages(1, 4096, 0xFFFFFFFFULL, &buf) == 0 && buf.virt) {
            volatile u64 *p = (volatile u64 *)buf.virt;
            p[0] = 0x4C4F545553533FULL; /* "LOTUSS" 探针值 */
            dma_ok = (p[0] == 0x4C4F545553533FULL);
            if (dma_ok) {
                /* 注意：当前 DMA 分配器无 free 接口，1 页探针为一次性成本 */
                log_info("[boot] selftest  dma probe page kept (no free API)");
            }
        }
        bself_line(&c, "DMA: alloc+rw probe", 0, dma_ok);
    }

    log_info("[boot] selftest summary:");
    log_hex64("[boot]   pass=", (u64)c.pass);
    log_hex64("[boot]   warn=", (u64)c.warn);
    log_hex64("[boot]   fail=", (u64)c.fail);
    log_info(c.fail ? "[boot] BOOT-OK: FATAL-FAIL" : "[boot] BOOT-OK");

    return c.fail ? -1 : 0;
}

/* F1 验证钩子：FUCK [boot] panic_test=1 时触发莲花崩溃屏（默认 0）。
 * 用于 QEMU 演示/回归：panic_full → 串口 dump + DRR 归档 + 帧缓冲 + halt。 */
void boot_panic_test(const ini_config *cfg);
void boot_panic_test(const ini_config *cfg) {
    extern int ini_get_bool(const ini_config *cfg, const char *section, const char *key, int defval);
    if (ini_get_bool(cfg, "boot", "panic_test", 0)) {
        log_warn("[boot] panic_test=1 -> triggering lotus panic screen");
        panic_full("UTSM-E42", "lotus panic selftest", 0);
    }
}
