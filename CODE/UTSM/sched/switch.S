/* switch.S — SAS-R0-PCQ 上下文切换（xv6 式栈驱动）
 *
 * utsm_sched_switch(prev, next):
 *   保存 prev 的 callee-saved（rbp/rbx/r12-r15）到其栈上，把栈顶写入
 *   prev->arch.rsp（并同步 callee-saved 副本到 arch 字段供 DRR/调试读取），
 *   载入 next->arch.rsp 恢复其 callee-saved，ret。
 *
 *   两条恢复路径：
 *     - IRQ 抢占换出的任务：其栈上保留 isr_common 帧 + handler C 帧，
 *       ret 回到它上次 switch 调用点，最终 iretq 恢复原始上下文。
 *     - 新任务：栈上是我们伪造的 7 个 u64（sched.c utsm_task_create），
 *       ret 直接落 utsm_task_trampoline。
 *
 * 寄存器约定（System V）：rdi=prev, rsi=next；volatile 由 C 约定视为破坏。
 *
 * TCB 布局偏移由 sched.c 的 _Static_assert 锁定（arch@80）。
 */

.set TCB_ARCH_RSP,   80
.set TCB_ARCH_RBX,  104
.set TCB_ARCH_RBP,  112
.set TCB_ARCH_R12,  120
.set TCB_ARCH_R13,  128
.set TCB_ARCH_R14,  136
.set TCB_ARCH_R15,  144

.text
.globl utsm_sched_switch
.type utsm_sched_switch, @function
utsm_sched_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    /* (%rsp)=r15 +8=r14 +16=r13 +24=r12 +32=rbx +40=rbp */
    movq  %rsp, TCB_ARCH_RSP(%rdi)      # prev->arch.rsp
    movq  0(%rsp), %rax
    movq  %rax, TCB_ARCH_R15(%rdi)      # callee-saved 副本（DRR 快照/调试用）
    movq  8(%rsp), %rax
    movq  %rax, TCB_ARCH_R14(%rdi)
    movq  16(%rsp), %rax
    movq  %rax, TCB_ARCH_R13(%rdi)
    movq  24(%rsp), %rax
    movq  %rax, TCB_ARCH_R12(%rdi)
    movq  32(%rsp), %rax
    movq  %rax, TCB_ARCH_RBX(%rdi)
    movq  40(%rsp), %rax
    movq  %rax, TCB_ARCH_RBP(%rdi)

    movq  TCB_ARCH_RSP(%rsi), %rsp      # 换栈到 next
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret                                  # 新任务 → trampoline；其余 → 上次切出点
.size utsm_sched_switch, . - utsm_sched_switch

.globl utsm_task_trampoline
.type utsm_task_trampoline, @function
utsm_task_trampoline:
    sti                                  # 新任务首次运行：开中断（rflags 帧后续由 iretq 维护）
    movq  %rbx, %rdi                     # rbx = task slot（伪造帧 sp[4]）
    call  utsm_task_main                 # entry(arg) + 自动 task_exit
    call  utsm_sched_task_exit           # 兜底（utsm_task_main 正常不返回）
1:  jmp 1b
.size utsm_task_trampoline, . - utsm_task_trampoline
