/* xj380_boot.c — VMCS configuration and vmlaunch for OpenXJ380 guest.
 *
 * 参照 linux_boot.c 模式。XJ380 内核入口 KernelMain 是 C++ extern "C"，
 * 以 SysV ABI 传 3 个引用参数（退化为指针）：
 *   RDI = &FrameBufferConfig   （HHDM 虚拟地址）
 *   RSI = &EFI_SYSTEM_TABLE    （伪表，RuntimeServices=NULL，内核仅存指针）
 *   RDX = &BOOT_CONFIG         （HHDM 虚拟地址）
 * RIP = 0xFFFFFFFF80000000（guest 虚拟地址，由 guest 页表内核区映射）。
 *
 * guest 终止（异常/三重故障/0xCF9 重启）后本函数返回，host 恢复。
 */

#include <utsm/xj380_loader.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* VM-Exit 汇编入口与 C 分发器（vmm.c / vmexit_asm.S） */
extern void vmx_vm_exit_handler(void);
extern int vmexit_dispatch(void);
extern u64 vmexit_get_count(void);

extern u8 g_host_stack[8192];

/* 当前运行的 guest 是否为 XJ380（vmm.h 声明，vmexit.c handle_hlt 检查）。
 * 定义在本模块：XJ380 launch 置 1，guest 终止后清 0。 */
volatile int g_xj380_guest_active;

/* CR 辅助 */
static inline u64 read_cr0_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr3_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr4_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static u64 xj380_adjust_control(u64 value, u32 msr) {
    u32 allowed0 = (u32)vmx_read_msr(msr);
    u32 allowed1 = (u32)(vmx_read_msr(msr) >> 32);
    u32 adjusted = (u32)value | allowed0;
    adjusted &= allowed1;
    return adjusted;
}

/* MSR bitmap（4KB，全零 = 不拦截任何 MSR，与 Linux guest 一致） */
static u8 g_xj380_msr_bitmap[4096] __attribute__((aligned(4096)));

/* ===== VMCS host state ===== */

static void xj380_vmcs_setup_host_state(void) {
    vmx_vmcs_write(VMCS_HOST_CR0, read_cr0_local() | CR0_PE | CR0_NE | CR0_PG);
    vmx_vmcs_write(VMCS_HOST_CR3, read_cr3_local());
    vmx_vmcs_write(VMCS_HOST_CR4, read_cr4_local() | CR4_VMXE);

    vmx_vmcs_write(VMCS_HOST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_HOST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_HOST_GS_BASE, 0);

    {
        struct __attribute__((packed)) { u16 limit; u64 base; } gdtr, idtr;
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        __asm__ volatile("sidt %0" : "=m"(idtr));
        vmx_vmcs_write(VMCS_HOST_GDTR_BASE, gdtr.base);
        vmx_vmcs_write(VMCS_HOST_IDTR_BASE, idtr.base);

        u16 tr_sel;
        u64 tr_base;
        __asm__ volatile("str %0" : "=r"(tr_sel));
        u64 gdt_base = gdtr.base;
        u64 *tss_desc = (u64 *)(gdt_base + (tr_sel & 0xFFF8));
        u64 low = tss_desc[0];
        u64 high = tss_desc[1];
        tr_base = (low >> 16) & 0xFFFFFF;
        tr_base |= ((low >> 32) & 0xFF) << 24;
        tr_base |= (high & 0xFFFFFFFF) << 32;
        vmx_vmcs_write(VMCS_HOST_TR_SELECTOR, tr_sel);
        vmx_vmcs_write(VMCS_HOST_TR_BASE, tr_base);
    }

    {
        u64 sysenter_cs;
        __asm__ volatile("rdmsr" : "=a"(sysenter_cs) : "c"(0x174));
        vmx_vmcs_write(VMCS_HOST_SYSENTER_CS, sysenter_cs & 0xFFFFFFFF);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_ESP, 0);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_EIP, 0);
    }

    {
        u32 low, high;
        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
        u64 host_efer = ((u64)high << 32) | low;
        vmx_vmcs_write(VMCS_HOST_IA32_EFER, host_efer);
    }

    vmx_vmcs_write(VMCS_HOST_RSP, (u64)&g_host_stack[sizeof(g_host_stack) - 16]);
    vmx_vmcs_write(VMCS_HOST_RIP, (u64)vmx_vm_exit_handler);
}

/* ===== VMCS guest state ===== */

static void xj380_vmcs_setup_guest_state(const struct xj380_guest_info *gi) {
    vmx_vmcs_write(VMCS_GUEST_CR0, CR0_PE | CR0_NE | CR0_PG | CR0_WP);
    vmx_vmcs_write(VMCS_GUEST_CR3, gi->pgt_gpa);
    vmx_vmcs_write(VMCS_GUEST_CR4, CR4_PAE | CR4_PGE | CR4_PSE);

    vmx_vmcs_write(VMCS_GUEST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_GUEST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_SELECTOR, 0x18);

    vmx_vmcs_write(VMCS_GUEST_CS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_SS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_DS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_ES_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_BASE, gi->gdt_gpa + 0x100);
    vmx_vmcs_write(VMCS_GUEST_GDTR_BASE, gi->gdt_gpa);
    vmx_vmcs_write(VMCS_GUEST_IDTR_BASE, 0);

    vmx_vmcs_write(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_SS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_ES_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_FS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_GS_LIMIT, 0xFFFFFFFF);
    vmx_vmcs_write(VMCS_GUEST_LDTR_LIMIT, 0xFFFF);
    vmx_vmcs_write(VMCS_GUEST_TR_LIMIT, 0x67);
    vmx_vmcs_write(VMCS_GUEST_GDTR_LIMIT, 0x28);
    vmx_vmcs_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);

    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);   /* 64 位代码 L=1 */
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x82);   /* unusable */
    vmx_vmcs_write(VMCS_GUEST_TR_ACCESS, 0x8B);     /* busy 64-bit TSS */

    /* guest RIP = XJ380 内核入口（guest 虚拟地址，经内核区页表映射） */
    vmx_vmcs_write(VMCS_GUEST_RIP, gi->kernel_entry);
    vmx_vmcs_write(VMCS_GUEST_RSP, XJ380_BOOT_STACK_TOP);

    vmx_vmcs_write(VMCS_GUEST_RFLAGS, 0x2);  /* IF=0 */

    vmx_vmcs_write(VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA | EFER_NXE);

    vmx_vmcs_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_EIP, 0);

    vmx_vmcs_write(VMCS_GUEST_DR7, 0x400);
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

/* ===== VMCS controls ===== */

static void xj380_vmcs_setup_controls(u64 eptp) {
    u64 pin_want = PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING;
    if (vmx_preemption_timer_supported()) {
        pin_want |= PIN_VMX_PREEMPTION_TIMER;
    }
    u64 pin = xj380_adjust_control(pin_want, IA32_VMX_TRUE_PINBASED_CTLS);

    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING;
    cpu = xj380_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    u64 cpu2 = SEC_EXEC_ENABLE_EPT | SEC_EXEC_ENABLE_VPID;
    {
        u64 msr = vmx_read_msr(IA32_VMX_PROCBASED_CTLS2);
        u32 allowed1 = (u32)(msr >> 32);
        cpu2 &= allowed1;
        cpu2 |= SEC_EXEC_ENABLE_EPT;
    }

    u64 exit_ctrl = VM_EXIT_SAVE_DEBUG_CONTROLS
                  | VM_EXIT_HOST_ADDR_SPACE_SIZE
                  | VM_EXIT_LOAD_HOST_EFER;
    exit_ctrl = xj380_adjust_control(exit_ctrl, IA32_VMX_TRUE_EXIT_CTLS);

    u64 entry_ctrl = VM_ENTRY_LOAD_DEBUG_CONTROLS
                   | VM_ENTRY_IA32E_MODE_GUEST
                   | VM_ENTRY_LOAD_GUEST_EFER;
    entry_ctrl = xj380_adjust_control(entry_ctrl, IA32_VMX_TRUE_ENTRY_CTLS);

    vmx_vmcs_write(VMCS_PIN_BASED_VM_EXEC_CONTROL, pin);
    vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL, cpu);
    vmx_vmcs_write(VMCS_SECONDARY_VM_EXEC_CONTROL, cpu2);
    vmx_vmcs_write(VMCS_VM_EXIT_CONTROLS, exit_ctrl);
    vmx_vmcs_write(VMCS_VM_ENTRY_CONTROLS, entry_ctrl);

    {
        u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
        u64 msr_bitmap_phys = (u64)g_xj380_msr_bitmap - hhdm;
        vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);
    }

    vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    vmx_vmcs_write(VMCS_VPID, 3);  /* VPID 1=self-test, 2=Linux, 3=XJ380 */

    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP, (1ULL << 13) | (1ULL << 14) | (1ULL << 6) | (1ULL << 8));
}

/* ===== 传给 vmlaunch asm 的参数 ===== */
static volatile u64 g_xj380_rdi;
static volatile u64 g_xj380_rsi;
static volatile u64 g_xj380_rdx;

/* ===== XJ380 launch =====
 *
 * 前置条件：vmm_init() + xj380_loader_init()。
 * guest 终止后返回 0；vmlaunch 失败返回负值。 */
int xj380_launch(void) {
    const struct xj380_guest_info *gi = xj380_get_guest_info();
    if (!gi || !gi->loaded) {
        log_error("[XJ380] guest not loaded");
        return -1;
    }

    if (!vmm_is_ready()) {
        log_error("[XJ380] VMM not ready");
        return -2;
    }

    log_info("[XJ380] launch begin");

    /* 真机 EPT TLB 刷新：vmlaunch 前确保 EPT 映射可见 */
    ept_flush_ept();

    /* 重置 VM-Exit 状态 */
    g_guest_terminated = 0;
    g_last_exit_reason = 0xFFFFFFFFULL;
    g_linux_guest_active = 0;   /* XJ380 是当前 active guest */
    g_guest_parked = 0;
    g_xj380_guest_active = 1;

    /* 配置 VMCS */
    xj380_vmcs_setup_guest_state(gi);
    xj380_vmcs_setup_controls(vmm_get_eptp());
    xj380_vmcs_setup_host_state();

    /* 初始 arm preemption timer（1ms 后首次周期 exit） */
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum());

    /* KernelMain 3 个 SysV 参数（HHDM 虚拟地址） */
    g_xj380_rdi = XJ380_FBC_GPA + XJ380_HHDM_OFFSET;
    g_xj380_rsi = XJ380_FAKEEFI_GPA + XJ380_HHDM_OFFSET;
    g_xj380_rdx = XJ380_BOOTCFG_GPA + XJ380_HHDM_OFFSET;

    log_hex64("[XJ380] RIP=", gi->kernel_entry);
    log_hex64("[XJ380] RSP=", XJ380_BOOT_STACK_TOP);
    log_hex64("[XJ380] RDI(fbc)=", g_xj380_rdi);
    log_hex64("[XJ380] RSI(efi)=", g_xj380_rsi);
    log_hex64("[XJ380] RDX(bootcfg)=", g_xj380_rdx);
    log_hex64("[XJ380] CR3=", gi->pgt_gpa);
    log_hex64("[XJ380] GDTR=", gi->gdt_gpa);
    log_info("[XJ380] vmlaunch (host suspended until guest terminates)");

    int failed;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"
        "movq g_xj380_rdi(%%rip), %%rdi\n\t"
        "movq g_xj380_rsi(%%rip), %%rsi\n\t"
        "movq g_xj380_rdx(%%rip), %%rdx\n\t"
        "vmlaunch\n\t"
        /* ---- failure path ---- */
        "movl $1, %0\n\t"
        "jmp 2f\n\t"
        /* ---- post_guest: terminate path jmps here ---- */
        "1:\n\t"
        "movl $0, %0\n\t"
        "2:\n\t"
        : "=r"(failed)
        :: "rax", "rcx", "rdx", "rsi", "rdi",
           "r8", "r9", "r10", "r11", "memory"
    );

    if (failed) {
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[XJ380] vmlaunch failed, error=", error);
        g_xj380_guest_active = 0;
        return -3;
    }

    /* guest 终止，host 恢复 */
    g_xj380_guest_active = 0;
    log_info("[XJ380] guest terminated, host resumed");
    log_hex64("[XJ380] last exit reason=", g_last_exit_reason);
    log_hex64("[XJ380] vmexit count=", vmexit_get_count());
    return 0;
}
