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
#include <utsm/vmx.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>

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

    /* vmresume 控制流（与 linux_launch 的 vmlaunch 对称）：
     *   - 失败：执行下一条指令，failed=1
     *   - 成功：进入 guest，不返回。VM-Exit 后跳到 vmx_vm_exit_handler。
     *   - guest park 时，handler 恢复 saved_rsp 并 jmp 到 post_resume 标签。 */
    int failed;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"      /* 保存当前 RSP */
        "leaq 1f(%%rip), %%rax\n\t"                     /* 取 post_resume 标签地址 */
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"     /* 保存返回 RIP */
        "vmresume\n\t"                                  /* 唤醒 guest */
        /* ---- 失败路径 ---- */
        "movl $1, %0\n\t"
        "jmp 2f\n\t"
        /* ---- post_resume: park 路径 jmp 到这里 ---- */
        "1:\n\t"
        "movl $0, %0\n\t"
        "2:\n\t"
        : "=r"(failed)
        :: "rax", "rcx", "rdx", "rsi", "rdi",
           "r8", "r9", "r10", "r11", "memory"
    );

    /* 回到 host 上下文 */
    g_linux_guest_active = 0;
    g_guest_parked = 1;   /* guest 再次 park */

    if (failed) {
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[LINUX] vmresume failed, error=", error);
        return -2;
    }

    return 0;
}
