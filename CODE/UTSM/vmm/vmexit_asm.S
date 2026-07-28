/* VM-Exit assembly entry.
 *
 * On VM-Exit, CPU loads host state from VMCS:
 *   - host CR0/CR3/CR4, segment registers
 *   - host RSP = VMCS_HOST_RSP (top of g_host_stack)
 *   - host RIP = VMCS_HOST_RIP (this function)
 *
 * VMCS does NOT save guest GPRs (RAX-R15). They remain in the CPU registers
 * and must be saved explicitly if the C dispatcher wants to read or modify
 * them (e.g. for VMCALL hypercall arguments and return values).
 *
 * This function:
 *   1. Saves all 16 guest GPRs into g_guest_regs
 *   2. Calls vmm_vmexit_entry() (C dispatcher, returns void)
 *      - Internally calls vmexit_dispatch(); sets g_guest_terminated if !resume
 *   3. Restores all 16 guest GPRs from g_guest_regs (C may have modified rax)
 *   4. Checks g_guest_terminated:
 *      - 0: executes vmresume (GPRs = guest values, possibly modified by C)
 *           If vmresume fails, falls through to terminate
 *      - 1: restores saved host RSP, jmps to saved return RIP
 *
 * The saved RSP/RIP point into the vmlaunch inline asm block (in vmm_self_test
 * or linux_launch). After the jmp, C flow continues with failed=0.
 */

.section .text
.global vmx_vm_exit_handler
vmx_vm_exit_handler:
    /* Step 1: Save all 16 guest GPRs to g_guest_regs.
     * GPRs at this point hold guest values (VMCS doesn't save them). */
    movq %rax,  g_guest_regs + 0(%rip)
    movq %rbx,  g_guest_regs + 8(%rip)
    movq %rcx,  g_guest_regs + 16(%rip)
    movq %rdx,  g_guest_regs + 24(%rip)
    movq %rsi,  g_guest_regs + 32(%rip)
    movq %rdi,  g_guest_regs + 40(%rip)
    movq %rbp,  g_guest_regs + 48(%rip)
    movq %r8,   g_guest_regs + 56(%rip)
    movq %r9,   g_guest_regs + 64(%rip)
    movq %r10,  g_guest_regs + 72(%rip)
    movq %r11,  g_guest_regs + 80(%rip)
    movq %r12,  g_guest_regs + 88(%rip)
    movq %r13,  g_guest_regs + 96(%rip)
    movq %r14,  g_guest_regs + 104(%rip)
    movq %r15,  g_guest_regs + 112(%rip)

    /* Step 2: Align stack to 16 bytes and call C dispatcher.
     * The host RSP (from VMCS_HOST_RSP) is 16-byte aligned minus 8
     * (for the implicit return address), so we sub 8 for alignment. */
    subq $8, %rsp
    call vmm_vmexit_entry
    addq $8, %rsp

    /* Step 3: Restore all 16 guest GPRs from g_guest_regs.
     * C handlers may have modified fields (e.g. rax = hypercall return value). */
    movq  g_guest_regs + 0(%rip), %rax
    movq  g_guest_regs + 8(%rip), %rbx
    movq  g_guest_regs + 16(%rip), %rcx
    movq  g_guest_regs + 24(%rip), %rdx
    movq  g_guest_regs + 32(%rip), %rsi
    movq  g_guest_regs + 40(%rip), %rdi
    movq  g_guest_regs + 48(%rip), %rbp
    movq  g_guest_regs + 56(%rip), %r8
    movq  g_guest_regs + 64(%rip), %r9
    movq  g_guest_regs + 72(%rip), %r10
    movq  g_guest_regs + 80(%rip), %r11
    movq  g_guest_regs + 88(%rip), %r12
    movq  g_guest_regs + 96(%rip), %r13
    movq  g_guest_regs + 104(%rip), %r14
    movq  g_guest_regs + 112(%rip), %r15

    /* Step 4: Check terminate flag */
    movl g_guest_terminated(%rip), %eax
    testl %eax, %eax
    jnz .Lterminate

    /* Resume guest: GPRs are now restored (with any C modifications).
     * vmresume does not return on success — CPU enters guest. */
    vmresume
    /* If we reach here, vmresume failed (CF=1 or ZF=1). */
    movl $1, g_guest_terminated(%rip)
    /* Fall through to terminate */

.Lterminate:
    /* Terminate: restore saved RSP and jmp to saved return RIP.
     * The saved RSP is the caller's stack pointer before vmlaunch.
     * The saved RIP is the "1:" label in the vmlaunch inline asm,
     * which sets failed=0 and continues C flow. */
    movq g_saved_host_rsp(%rip), %rsp
    jmpq *g_saved_return_rip(%rip)
