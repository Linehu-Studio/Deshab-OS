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

#endif
