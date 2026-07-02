.section .text
.global _start
.type _start, @function
_start:
    cli
    cld
    lea kernel_stack_top(%rip), %rsp
    andq $-16, %rsp
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang

.section .bss
.align 16
.global kernel_stack
.global kernel_stack_top
kernel_stack:
    .skip 16384
kernel_stack_top:
