/* linux_resume.c — Linux guest park-and-resume 唤醒实现。
 *
 * linux_resume() 通过 vmresume 唤醒已 HLT park 的 Linux guest。
 * guest daemon 处理 IPC 请求后再次 HLT park，控制权返回 host。
 *
 * 控制流与 linux_launch() 的 vmlaunch 对称：
 *   - 保存 host RSP 和返回 RIP
 *   - 执行 vmresume（成功则进入 guest，不返回）
 *   - guest HLT → VM-Exit → handle_hlt park → g_guest_terminated=1
 *   - vmexit_asm.S 恢复 host RSP，jmp 到返回 RIP
 *   - linux_resume() 继续执行，返回 0
 */

#include <utsm/linux_resume.h>
#include <utsm/linux_xsave.h>
#include <utsm/vmx.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* Must be RIP-relative, not a stack slot: vmexit_asm restores guest GPRs
 * (including RBP) before jumping here, so "=m"(local) can #PF at CR2≈0. */
int g_linux_resume_failed;

int linux_is_parked(void) {
    return g_guest_parked ? 1 : 0;
}

int linux_resume(void) {
    if (!g_guest_parked) {
        log_warn("[LINUX] resume: guest not parked");
        return -1;
    }

    /* 设置 active 标志：handle_hlt 检查此标志决定 park（而非 terminate） */
    g_linux_guest_active = 1;
    g_guest_terminated = 0;
    g_guest_parked = 0;
    vmx_linux_timeslice_reset();
    linux_xsave_save_host();
    linux_xsave_load_guest();
    vmx_linux_prepare_entry();

    /* vmresume 控制流（与 linux_launch 的 vmlaunch 对称）。
     * clobber 全 GPR：从 park 跳回时 CPU 上是 guest 寄存器。 */
    g_linux_resume_failed = 1;
    __asm__ volatile(
        "pushq %%rbx\n\t"
        "pushq %%rbp\n\t"
        "pushq %%r12\n\t"
        "pushq %%r13\n\t"
        "pushq %%r14\n\t"
        "pushq %%r15\n\t"
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"
        "vmresume\n\t"
        "movl $1, g_linux_resume_failed(%%rip)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "movl $0, g_linux_resume_failed(%%rip)\n\t"
        "2:\n\t"
        "popq %%r15\n\t"
        "popq %%r14\n\t"
        "popq %%r13\n\t"
        "popq %%r12\n\t"
        "popq %%rbp\n\t"
        "popq %%rbx\n\t"
        :
        :
        : "rax", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "cc", "memory"
    );

    /* 回到 host 上下文 */
    g_linux_guest_active = 0;
    g_guest_parked = 1;
    vmx_linux_timeslice_disarm();
    linux_xsave_save_guest();
    linux_xsave_load_host();

    if (g_linux_resume_failed) {
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[LINUX] vmresume failed, error=", error);
        return -2;
    }

    return 0;
}
