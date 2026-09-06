#ifndef UTSM_VMM_H
#define UTSM_VMM_H

#include <utsm/types.h>

/* VMM 顶层 API：协调 VMX/EPT/VMCS/VMEXIT，启动 guest。
 *
 * Phase 1.1（当前）：vmm_init() 完成 VMX root 启用 + EPT 初始化，
 * vmm_self_test() 启动一个最小 guest（HLT 指令）验证 VMEXIT 路径。
 *
 * Phase 1.2+：vmm_launch_linux() 加载并启动 Linux 6.6 LTS guest。
 */

/* ===== Guest register save area =====
 *
 * VMCS does NOT save guest GPRs (RAX-R15) on VM-Exit. The assembly VM-Exit
 * handler (vmexit_asm.S) saves all 16 GPRs into this structure before
 * calling the C dispatcher, and restores them before vmresume.
 *
 * C handlers (e.g. hypercall.c) can read guest arguments from this structure
 * and write return values (e.g. g_guest_regs.rax = retval) which will be
 * loaded into the CPU before vmresume resumes the guest. */
struct vmexit_guest_regs {
    u64 rax;
    u64 rbx;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 rbp;
    u64 r8;
    u64 r9;
    u64 r10;
    u64 r11;
    u64 r12;
    u64 r13;
    u64 r14;
    u64 r15;
};

/* Global guest register save area (referenced by vmexit_asm.S).
 * Non-static: shared between vmm.c, vmexit.c, hypercall.c, and vmexit_asm.S. */
extern volatile struct vmexit_guest_regs g_guest_regs;

/* ===== Linux guest park-and-resume 状态 =====
 *
 * g_linux_guest_active: 1 = 当前运行的 guest 是 Linux guest。
 *   handle_hlt 检查此标志：Linux guest HLT 时 park（推进 RIP，exit-to-host），
 *   self-test guest HLT 时 terminate。
 * g_guest_parked: 1 = Linux guest 已 HLT 驻留，可被 linux_resume() 唤醒。
 *   linux_compat_service 通过此标志判断 guest 是否就绪。
 *
 * g_xj380_guest_active: 1 = 当前运行的 guest 是 OpenXJ380 guest。
 *   handle_hlt 检查此标志：XJ380 guest HLT 时推进 RIP 并立即 vmresume
 *   （继续执行，不 park 不 terminate）。同一时刻只有一个 guest active。 */
extern volatile int g_linux_guest_active;
extern volatile int g_guest_parked;
extern volatile int g_xj380_guest_active;

/* ===== VM-Exit host 恢复点（vmm.c 定义，vmm.c / linux_boot.c / linux_resume.c 共用） =====
 *
 * g_saved_host_rsp:   vmlaunch/vmresume 前保存的 host RSP。VM-Exit 处理器
 *                     终止 guest 时恢复此 RSP 并 jmp 到 g_saved_return_rip。
 * g_saved_return_rip: vmlaunch/vmresume 后的返回地址（post_guest / post_resume 标签）。
 * g_guest_terminated: 1 = guest 已终止（不再 vmresume）。dispatch 返回 0 时置位。
 * g_last_exit_reason: 最近一次 VM-Exit 的 reason（vmexit_asm.S 路径写入）。
 *
 * 这些符号必须是非 static 全局变量，因为 vmexit_asm.S 通过 RIP 相对寻址引用。 */
extern volatile u64 g_saved_host_rsp;
extern volatile u64 g_saved_return_rip;
extern volatile int g_guest_terminated;
extern volatile u64 g_last_exit_reason;

/* ===== VMM API ===== */

/* 初始化 VMM：VMX root 模式 + EPT 表 + 分配 VMCS。
 * 返回 0 成功，负值失败。 */
int vmm_init(void);

/* 关闭 VMM：vmxoff。 */
void vmm_shutdown(void);

/* Phase 1.1 自检：启动最小 guest 验证 VMX 路径。
 * guest 代码为内嵌的 HLT 指令，预期触发 HLT VMEXIT。
 * 返回 0 成功。 */
int vmm_self_test(void);

/* VMM 状态查询 */
int vmm_is_ready(void);
u64 vmm_get_eptp(void);
/* P8.5: VMCS 物理地址（linux_launch 前 VMCLEAR+VMPTRLD 用） */
u64 vmm_get_vmcs_phys(void);

#endif
