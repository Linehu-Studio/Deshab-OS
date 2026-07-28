/* linux_boot.c — VMCS configuration and vmlaunch for Linux 6.6 guest.
 *
 * Configures the VMCS guest state for 64-bit Linux entry (startup_64):
 *   - RIP = kernel 64-bit entry point
 *   - RSI = boot_params GPA (Linux calling convention)
 *   - RSP = guest stack top
 *   - CR3 = guest page tables GPA (identity-mapped 4GB)
 *   - GDT = guest GDT GPA (64-bit code/data/TSS)
 *   - EFER.LME | EFER.LMA | EFER.NXE
 *
 * After vmlaunch, the VM-Exit handler (vmexit.c) processes exits and
 * resumes the guest.
 */

#include <utsm/linux_loader.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* VM-Exit assembly entry and C dispatcher (from vmm.c / vmexit_asm.S) */
extern void vmx_vm_exit_handler(void);
extern int vmexit_dispatch(void);
extern u64 vmexit_get_count(void);

/* Globals shared with vmexit_asm.S (must match vmm.c) */
extern volatile u64 g_saved_host_rsp;
extern volatile u64 g_saved_return_rip;
extern volatile int g_guest_terminated;
extern volatile u64 g_last_exit_reason;

/* Host stack for VM-Exit (reuse the one from vmm.c) */
extern u8 g_host_stack[8192];

/* CR helpers */
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

static u64 vmx_adjust_control(u64 value, u32 msr) {
    u32 allowed0 = (u32)vmx_read_msr(msr);
    u32 allowed1 = (u32)(vmx_read_msr(msr) >> 32);
    u32 adjusted = (u32)value | allowed0;
    adjusted &= allowed1;
    return adjusted;
}

/* MSR bitmap (4KB, all zeros = don't intercept any MSR) */
static u8 g_linux_msr_bitmap[4096] __attribute__((aligned(4096)));

/* ===== VMCS host state (same as vmm.c, but for Linux guest) ===== */

static void linux_vmcs_setup_host_state(void) {
    vmx_vmcs_write(VMCS_HOST_CR0, read_cr0_local() | CR0_PE | CR0_NE | CR0_PG);
    vmx_vmcs_write(VMCS_HOST_CR3, read_cr3_local());
    vmx_vmcs_write(VMCS_HOST_CR4, read_cr4_local() | CR4_VMXE);

    /* Host segment selectors: UTSM GDT 0x08 (code) / 0x10 (data) */
    vmx_vmcs_write(VMCS_HOST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_HOST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_HOST_GS_BASE, 0);

    /* Host GDT/IDT + TR */
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

    /* Host SYSENTER */
    {
        u64 sysenter_cs;
        __asm__ volatile("rdmsr" : "=a"(sysenter_cs) : "c"(0x174));
        vmx_vmcs_write(VMCS_HOST_SYSENTER_CS, sysenter_cs & 0xFFFFFFFF);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_ESP, 0);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_EIP, 0);
    }

    /* Host EFER */
    {
        u32 low, high;
        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
        u64 host_efer = ((u64)high << 32) | low;
        vmx_vmcs_write(VMCS_HOST_IA32_EFER, host_efer);
    }

    /* Host RSP/RIP: VM-Exit handler */
    vmx_vmcs_write(VMCS_HOST_RSP, (u64)&g_host_stack[sizeof(g_host_stack) - 16]);
    vmx_vmcs_write(VMCS_HOST_RIP, (u64)vmx_vm_exit_handler);
}

/* ===== VMCS guest state for Linux ===== */

static void linux_vmcs_setup_guest_state(const struct linux_guest_info *gi) {
    /* CR0/CR3/CR4: long mode with paging */
    vmx_vmcs_write(VMCS_GUEST_CR0, CR0_PE | CR0_NE | CR0_PG | CR0_WP);
    /* CR3 points to guest PML4 (GPA) */
    vmx_vmcs_write(VMCS_GUEST_CR3, gi->pgt_gpa);
    vmx_vmcs_write(VMCS_GUEST_CR4, CR4_PAE | CR4_PGE | CR4_PSE);

    /* Segment selectors from guest GDT */
    vmx_vmcs_write(VMCS_GUEST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_GUEST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_SELECTOR, 0x18);

    /* Segment bases: flat model, TR points to TSS in GDT page */
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

    /* Segment limits */
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

    /* Access rights */
    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);  /* 64-bit code, L=1 */
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x82);  /* unusable */
    vmx_vmcs_write(VMCS_GUEST_TR_ACCESS, 0x8B);    /* busy 64-bit TSS */

    /* Guest RIP = Linux 64-bit entry (startup_64) */
    vmx_vmcs_write(VMCS_GUEST_RIP, gi->kernel_entry);

    /* Guest RSP = top of stack page */
    vmx_vmcs_write(VMCS_GUEST_RSP, LINUX_GUEST_STACK_GPA + 0x1000);

    /* NOTE: RSI (boot_params address) is NOT a VMCS field.
     * VMX does not save/restore GPRs (RAX-R15) in VMCS.
     * RSI is set via the inline asm launch stub in linux_launch(). */

    vmx_vmcs_write(VMCS_GUEST_RFLAGS, 0x2);  /* interrupts disabled */

    /* Guest EFER: long mode active */
    vmx_vmcs_write(VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA | EFER_NXE);

    /* Guest SYSENTER (unused but required by VMX) */
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_EIP, 0);

    /* Guest DR7, activity state, interruptibility */
    vmx_vmcs_write(VMCS_GUEST_DR7, 0x400);
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

/* ===== VMCS controls for Linux ===== */

static void linux_vmcs_setup_controls(u64 eptp) {
    u64 pin = vmx_adjust_control(PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING,
                                 IA32_VMX_TRUE_PINBASED_CTLS);

    /* CPU-based: exit on HLT, INVLPG, MWAIT, IO, use MSR bitmap,
     * activate secondary controls. Don't intercept RDTSC (Linux needs it). */
    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING;
    cpu = vmx_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    /* Secondary: EPT + VPID */
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
    exit_ctrl = vmx_adjust_control(exit_ctrl, IA32_VMX_TRUE_EXIT_CTLS);

    u64 entry_ctrl = VM_ENTRY_LOAD_DEBUG_CONTROLS
                   | VM_ENTRY_IA32E_MODE_GUEST
                   | VM_ENTRY_LOAD_GUEST_EFER;
    entry_ctrl = vmx_adjust_control(entry_ctrl, IA32_VMX_TRUE_ENTRY_CTLS);

    vmx_vmcs_write(VMCS_PIN_BASED_VM_EXEC_CONTROL, pin);
    vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL, cpu);
    vmx_vmcs_write(VMCS_SECONDARY_VM_EXEC_CONTROL, cpu2);
    vmx_vmcs_write(VMCS_VM_EXIT_CONTROLS, exit_ctrl);
    vmx_vmcs_write(VMCS_VM_ENTRY_CONTROLS, entry_ctrl);

    /* MSR bitmap */
    {
        u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
        u64 msr_bitmap_phys = (u64)g_linux_msr_bitmap - hhdm;
        vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);
    }

    vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    vmx_vmcs_write(VMCS_VPID, 2);  /* VPID 1 = self-test, VPID 2 = Linux */

    /* Exception bitmap: catch #GP, #PF, #UD, #DF (triple fault handled separately) */
    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP, (1ULL << 13) | (1ULL << 14) | (1ULL << 6) | (1ULL << 8));
}

/* ===== Global for passing RSI to launch asm ===== */
static volatile u64 g_linux_rsi;

/* ===== Linux launch =====
 *
 * This function is called after vmm_init() + linux_loader_init().
 * It configures the VMCS for the Linux guest and performs vmlaunch.
 *
 * NOTE: VMX general registers (RAX-R15) are NOT saved in VMCS.
 * We use inline assembly to set RSI = boot_params GPA before vmlaunch.
 *
 * Returns 0 on successful guest termination, negative on error.
 */
int linux_launch(void) {
    const struct linux_guest_info *gi = linux_get_guest_info();
    if (!gi || !gi->loaded) {
        log_error("[LINUX] guest not loaded");
        return -1;
    }

    if (!vmm_is_ready()) {
        log_error("[LINUX] VMM not ready");
        return -2;
    }

    log_info("[LINUX] launch begin");

    /* Reset VM-Exit state */
    g_guest_terminated = 0;
    g_last_exit_reason = 0xFFFFFFFFULL;

    /* Configure VMCS */
    linux_vmcs_setup_guest_state(gi);
    linux_vmcs_setup_controls(vmm_get_eptp());
    linux_vmcs_setup_host_state();

    /* Set RSI for Linux boot_params (passed via global, loaded in asm) */
    g_linux_rsi = gi->bootparams_gpa;

    log_hex64("[LINUX] RIP=", gi->kernel_entry);
    log_hex64("[LINUX] RSP=", LINUX_GUEST_STACK_GPA + 0x1000);
    log_hex64("[LINUX] RSI=", gi->bootparams_gpa);
    log_hex64("[LINUX] CR3=", gi->pgt_gpa);
    log_hex64("[LINUX] GDTR=", gi->gdt_gpa);
    log_info("[LINUX] vmlaunch");

    /* vmlaunch with RSI = boot_params.
     * Same control flow pattern as vmm_self_test:
     *   - On failure: failed=1, falls through
     *   - On success: enters guest, VM-Exit → vmx_vm_exit_handler
     *   - On terminate: handler restores RSP, jmps to post_guest label */
    int failed;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"
        "movq g_linux_rsi(%%rip), %%rsi\n\t"   /* RSI = boot_params GPA */
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
        log_hex64("[LINUX] vmlaunch failed, error=", error);
        return -3;
    }

    /* Guest terminated */
    log_info("[LINUX] guest terminated");
    log_hex64("[LINUX] last exit reason=", g_last_exit_reason);
    log_hex64("[LINUX] vmexit count=", vmexit_get_count());

    return 0;
}
