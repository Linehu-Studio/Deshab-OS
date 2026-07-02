void arch_halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
