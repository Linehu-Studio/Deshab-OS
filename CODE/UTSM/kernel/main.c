#include <utsm/log.h>
#include <utsm/arena.h>
#include <utsm/utsm.h>
#include <utsm/drr.h>
#include <utsm/dkm.h>
#include <utsm/idt.h>
#include <utsm/dsk.h>
#include <utsm/block.h>
#include <utsm/net.h>

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

    log_info("[UTSM] drr stub init begin");
    drr_stub_init();
    log_info("[UTSM] drr stub init ok");

    log_info("[UTSM] core init begin");
    utsm_init();
    log_info("[UTSM] init ok");

    dkm_init();
    dkm_fill_platform_info();
    dsm_load_by_manifest();

    int result = utsm_selftest_run();
    if (result == 0) {
        log_info("[UTSM] SELFTEST PASS");
        if (dsk_load_and_jump() != 0) {
            log_error("[UTSM] DSK jump failed");
        }
    } else {
        log_error("[UTSM] SELFTEST FAIL");
    }

    arch_halt_forever();
}
