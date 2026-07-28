#include <utsm/log.h>
#include <utsm/arena.h>
#include <utsm/utsm.h>
#include <utsm/drr.h>
#include <utsm/dkm.h>
#include <utsm/idt.h>
#include <utsm/dsk.h>
#include <utsm/block.h>
#include <utsm/net.h>
#include <utsm/ipc_shm.h>

void arch_halt_forever(void);

void kernel_main(void) {
    serial_init();
    log_info("[UTSM] boot");

    idt_init();

    log_info("[UTSM] arena init begin");
    arena_init();
    log_info("[UTSM] arena init ok");

    log_info("[UTSM] dma init begin");
    dma_init();
    log_info("[UTSM] dma init ok");

    log_info("[UTSM] net registry init begin");
    net_init();
    log_info("[UTSM] net registry init ok");

    log_info("[UTSM] drr init begin");
    drr_init();
    log_info("[UTSM] drr init ok");

    log_info("[UTSM] core init begin");
    utsm_init();
    log_info("[UTSM] init ok");

    dkm_init();
    dkm_fill_platform_info();
    dsm_load_by_manifest();

    int result = utsm_selftest_run();
    if (result == 0) {
        log_info("[UTSM] SELFTEST PASS");

        /* Phase 1.1: VMM (VMX/EPT) 初始化 + 自检
         * 若 VMX 不可用（如 QEMU 未启用），仅警告，不阻断 DSK 启动 */
        if (vmm_init() == 0) {
            log_info("[UTSM] VMM init ok");
            int vmm_st = vmm_self_test();
            if (vmm_st == 0) {
                log_info("[UTSM] VMM self-test PASS");

                /* Phase 1.2: Linux 加载器 + 启动
                 * 若 bzImage 模块存在，加载并启动 Linux guest。
                 * 若无 bzImage，跳过 Linux 启动，继续 DSK 流程。 */
                if (linux_loader_init() == 0) {
                    log_info("[UTSM] Linux loader init ok");

                    /* Phase 1.3: Initialize IPC shared memory before
                     * launching Linux. Allocates 1MB, initializes ring
                     * buffers, and EPT-maps at GPA 0x04000000 so the
                     * Linux guest can access it via UTSM_HCALL_SHM_INFO. */
                    if (ipc_shm_init() == 0) {
                        log_info("[UTSM] IPC shm init ok");
                    } else {
                        log_error("[UTSM] IPC shm init failed (non-fatal)");
                    }

                    int lin_st = linux_launch();
                    if (lin_st == 0) {
                        log_info("[UTSM] Linux guest terminated");
                    } else {
                        log_error("[UTSM] Linux launch failed");
                        log_hex64("[UTSM] Linux st=", (u64)(i64)lin_st);
                    }
                } else {
                    log_warn("[UTSM] Linux loader unavailable (no bzImage)");
                }
            } else {
                log_error("[UTSM] VMM self-test FAIL");
                log_hex64("[UTSM] VMM st=", (u64)(i64)vmm_st);
            }
        } else {
            log_warn("[UTSM] VMM unavailable (VMX not supported or disabled)");
        }

        if (dsk_load_and_jump() != 0) {
            log_error("[UTSM] DSK jump failed");
        }
    } else {
        log_error("[UTSM] SELFTEST FAIL");
    }

    arch_halt_forever();
}
