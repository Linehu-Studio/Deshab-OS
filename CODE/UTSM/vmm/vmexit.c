#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/hypercall.h>
#include <utsm/ipc_shm.h>
#include <utsm/log.h>
#include <utsm/types.h>

/* VM-Exit 处理器：从 VMCS 读取 exit reason 与 guest 状态，分发处理。
 *
 * Phase 1.1：处理 HLT（终止 guest）、EPT violation（动态映射）、异常。
 * Phase 1.3：增加 VMCALL hypercall 处理（读 guest GPR、分发、写返回值）。
 */

extern void vmx_vmresume_wrapper(void);

static u64 g_vmexit_count;

static void log_exit_diagnostics(u64 reason, u64 qualification, u64 rip, u64 len) {
    log_hex64("[VMEXIT] reason=", reason);
    log_hex64("[VMEXIT] qual=", qualification);
    log_hex64("[VMEXIT] rip=", rip);
    log_hex64("[VMEXIT] ilen=", len);
}

/* 处理 HLT：
 * - self-test guest：终止（guest 完成测试）
 * - Linux guest：park（推进 RIP 越过 HLT，exit-to-host，设 g_guest_parked=1）
 *   host 调用 linux_resume() 时 vmresume 唤醒 guest 从 HLT 之后继续执行。 */
static int handle_hlt(u64 rip, u64 instr_len, int *out_resume) {
    *out_resume = 0;   /* 两种情况都 exit-to-host */
    if (g_linux_guest_active) {
        /* Linux guest: park — 推进 RIP 越过 HLT 指令 */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        g_guest_parked = 1;
        log_info("[VMEXIT] HLT - Linux guest parked");
    } else {
        /* self-test guest: 终止 */
        log_info("[VMEXIT] HLT - guest halted");
    }
    return 0;
}

/* 处理 EPT violation：动态映射缺失的 GPA。
 * Phase 1.1：1:1 映射缺失的页面，权限 RWX。 */
static int handle_ept_violation(u64 qualification, int *out_resume) {
    u64 gpa = vmx_vmcs_read(VMCS_GUEST_PHYSICAL_ADDR);
    u64 page = gpa & ~(EPT_PAGE_SIZE - 1);
    log_hex64("[VMEXIT] EPT violation gpa=", gpa);
    (void)qualification;
    /* 1:1 映射该页（假设 HPA=GPA） */
    if (ept_identity_map(page, EPT_PAGE_SIZE, EPT_RWX) != 0) {
        log_error("[VMEXIT] failed to map EPT page");
        *out_resume = 0;
        return -1;
    }
    *out_resume = 1;
    return 0;
}

/* 处理通用异常：打印诊断信息，终止 guest。 */
static int handle_exception(u64 qualification, u64 rip, int *out_resume) {
    u64 intr_info = vmx_vmcs_read(0x4404);  /* VM_EXIT_INTR_INFO */
    log_hex64("[VMEXIT] exception intr_info=", intr_info);
    log_hex64("[VMEXIT] exception qual=", qualification);
    log_hex64("[VMEXIT] exception rip=", rip);
    *out_resume = 0;
    return 0;
}

/* VM-Exit 主分发器。从 vmx_vm_exit_handler（汇编入口）调用。
 * 返回 1 表示 vmresume；0 表示终止 guest。 */
int vmexit_dispatch(void) {
    u64 reason = vmx_vmcs_read(VMCS_EXIT_REASON) & 0xFFFF;
    u64 qualification = vmx_vmcs_read(VMCS_EXIT_QUALIFICATION);
    u64 rip = vmx_vmcs_read(VMCS_GUEST_RIP);
    u64 instr_len = vmx_vmcs_read(VMCS_INSTRUCTION_LENGTH);
    int resume = 0;

    g_vmexit_count++;

    switch (reason) {
    case EXIT_HLT:
        handle_hlt(rip, instr_len, &resume);
        break;
    case EXIT_EPT_VIOLATION:
        handle_ept_violation(qualification, &resume);
        break;
    case EXIT_EXCEPTION_NMI:
        handle_exception(qualification, rip, &resume);
        break;
    case EXIT_TRIPLE_FAULT:
        log_error("[VMEXIT] triple fault");
        resume = 0;
        break;
    case EXIT_CPUID:
        /* 简化：让 guest 通过 CPUID，前进 RIP。Phase 1.2+ 完整模拟。 */
        log_info("[VMEXIT] CPUID - skip");
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_IO_INSTRUCTION:
        /* 简化：跳过 IO 指令。Phase 1.4 virtio-console 完整处理。 */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_RDMSR:
    case EXIT_WRMSR:
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_VMCALL:
        /* VMCALL hypercall: read guest GPRs from g_guest_regs (saved by
         * vmexit_asm.S), dispatch to the hypercall handler, and write
         * the return value into g_guest_regs.rax. The handler also
         * advances GUEST_RIP past the VMCALL instruction. */
        {
            int hcall_resume = hypercall_handle(
                g_guest_regs.rax, g_guest_regs.rdi,
                g_guest_regs.rsi, g_guest_regs.rdx,
                rip, instr_len);
            resume = hcall_resume;
        }
        break;
    default:
        log_exit_diagnostics(reason, qualification, rip, instr_len);
        resume = 0;
        break;
    }

    (void)qualification;
    return resume;
}

u64 vmexit_get_count(void) { return g_vmexit_count; }
