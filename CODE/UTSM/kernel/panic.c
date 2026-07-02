#include <utsm/log.h>

void arch_halt_forever(void);

void panic(const char *msg) {
    log_error("[PANIC]");
    log_error(msg);
    arch_halt_forever();
}
