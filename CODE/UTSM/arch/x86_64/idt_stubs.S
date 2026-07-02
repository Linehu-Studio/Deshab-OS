/* IDT assembly stubs — SAS-R0 interrupt vectors */
.section .text

.macro isr_no_err n
.global isr_stub_\n
.type isr_stub_\n, @function
isr_stub_\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro isr_err n
.global isr_stub_\n
.type isr_stub_\n, @function
isr_stub_\n:
    pushq $\n
    jmp isr_common
.endm

/* Exceptions 0-31 */
isr_no_err 0;  isr_no_err 1;  isr_no_err 2;  isr_no_err 3
isr_no_err 4;  isr_no_err 5;  isr_no_err 6;  isr_no_err 7
isr_err    8;  isr_no_err 9;  isr_err    10; isr_err    11
isr_err    12; isr_err    13; isr_err    14; isr_no_err 15
isr_no_err 16; isr_err    17; isr_no_err 18; isr_no_err 19
isr_no_err 20; isr_no_err 21; isr_no_err 22; isr_no_err 23
isr_no_err 24; isr_no_err 25; isr_no_err 26; isr_no_err 27
isr_no_err 28; isr_no_err 29; isr_err    30; isr_no_err 31

/* IRQ 32-47 */
isr_no_err 32; isr_no_err 33; isr_no_err 34; isr_no_err 35
isr_no_err 36; isr_no_err 37; isr_no_err 38; isr_no_err 39
isr_no_err 40; isr_no_err 41; isr_no_err 42; isr_no_err 43
isr_no_err 44; isr_no_err 45; isr_no_err 46; isr_no_err 47

/* Common ISR */
isr_common:
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %rcx
    pushq %rax
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11

    movq %rsp, %rdi
    call idt_handler

    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rax
    popq %rcx
    popq %rdx
    popq %rsi
    popq %rdi
    addq $16, %rsp
    iretq
