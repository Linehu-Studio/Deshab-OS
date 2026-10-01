/* linux_boot.c — VMCS configuration and vmlaunch for Linux 6.6 guest.
 *
 * Configures the VMCS guest state for 64-bit Linux entry (startup_64):
 *   - RIP = kernel 64-bit entry point
 *   - RSI = boot_params GPA (Linux calling convention)
 *   - RSP = guest stack top
 *   - CR3 = guest page tables GPA (identity-mapped 4GB)
 *   - GDT = guest GDT GPA (64-bit code/data/TSS)
 *   - EFER.SCE | EFER.LME | EFER.LMA | EFER.NXE
 *
 * After vmlaunch, the VM-Exit handler (vmexit.c) processes exits and
 * resumes the guest.
 */

#include <utsm/linux_loader.h>
#include <utsm/linux_xsave.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/virtio_mmio.h>
#include <utsm/ipc_shm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* VM-Exit assembly entry and C dispatcher (from vmm.c / vmexit_asm.S) */
extern void vmx_vm_exit_handler(void);
extern int vmexit_dispatch(void);
extern u64 vmexit_get_count(void);

/* g_saved_host_rsp / g_saved_return_rip / g_guest_terminated /
 * g_last_exit_reason / g_linux_guest_active / g_guest_parked /
 * g_guest_regs / g_host_stack 通过 <utsm/vmm.h> 声明（vmm.c 定义）。 */
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

/* MSR bitmap (4KB, all zeros = don't intercept any MSR) */
static u8 g_linux_msr_bitmap[4096] __attribute__((aligned(4096)));

int g_linux_cet_vmcs;
int g_linux_cet_force;
static u64 g_host_ucet;

struct vmx_msr_entry {
    u32 index;
    u32 reserved;
    u64 data;
} __attribute__((packed));
_Static_assert(sizeof(struct vmx_msr_entry) == 16, "VMX MSR entry is 16 bytes");

static struct vmx_msr_entry g_linux_msr_entry_load[1] __attribute__((aligned(16)));
static u32 g_linux_msr_entry_count;

void linux_cet_restore_host(void) {
    if (!g_linux_cet_force) return;
    vmx_write_msr(0x6A0, g_host_ucet);
}

void linux_cet_force_guest_off(void) {
    if (!g_linux_cet_force) return;
    vmx_write_msr(0x6A0, 0);
}

void linux_cet_vmcs_sync_host(void) {
    u64 scet, ist, ssp = 0;
    if (!g_linux_cet_vmcs) return;
    scet = vmx_read_msr(0x6A2);
    ist = vmx_read_msr(0x6A8);
    if (scet & 1ULL) {
        __asm__ volatile(".byte 0xf3, 0x48, 0x0f, 0x1e, 0xc8" : "=a"(ssp));
    }
    vmx_vmcs_write(VMCS_HOST_S_CET, scet);
    vmx_vmcs_write(VMCS_HOST_SSP, ssp);
    vmx_vmcs_write(VMCS_HOST_INTR_SSP_TABLE, ist);
}

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
    linux_cet_vmcs_sync_host();
}

/* ===== VMCS guest state for Linux ===== */

static void linux_vmcs_setup_guest_state(const struct linux_guest_info *gi) {
    /* CR0/CR3/CR4: long mode with paging.
     * P8.5: CR4 必须含 VMXE（IA32_VMX_CR4_FIXED0=0x2000 强制，
     * 旧代码漏掉会触发 VM-entry invalid guest state）。 */
    /* ET/MP + OSFXSR so the decompressor/kernel can use FXSAVE/SSE
     * before it programs CR0/CR4 itself. Missing OSFXSR is #UD with
     * IDTR=0, which becomes a triple fault. */
    u64 cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_PG | CR0_WP;
    u64 cr4 = CR4_VMXE | CR4_PAE | CR4_PGE | CR4_PSE |
              CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (linux_xsave_available())
        cr4 |= CR4_OSXSAVE;
    vmx_vmcs_write(VMCS_GUEST_CR0, cr0);
    /* CR3 points to guest PML4 (GPA) */
    vmx_vmcs_write(VMCS_GUEST_CR3, gi->pgt_gpa);
    vmx_vmcs_write(VMCS_GUEST_CR4, cr4);

    vmx_vmcs_write(VMCS_GUEST_S_CET, 0);
    vmx_vmcs_write(VMCS_GUEST_SSP, 0);
    vmx_vmcs_write(VMCS_GUEST_INTR_SSP_TABLE, 0);

    /* startup_64 writes CR4 = PAE|PGE and drops VMXE. In VMX non-root
     * that is #GP; the decompressor IDT then #DFs. Own FIXED0 bits
     * (VMXE) so the write updates the shadow only. */
    {
        u64 cr0_fixed0 = vmx_read_msr(IA32_VMX_CR0_FIXED0) & 0xFFFFFFFFULL;
        u64 cr0_fixed1 = vmx_read_msr(IA32_VMX_CR0_FIXED1) & 0xFFFFFFFFULL;
        u64 cr4_fixed0 = vmx_read_msr(IA32_VMX_CR4_FIXED0) & 0xFFFFFFFFULL;
        u64 cr0_mask = cr0_fixed0 | (~cr0_fixed1 & 0xFFFFFFFFULL);
        vmx_vmcs_write(VMCS_CR0_GUEST_HOST_MASK, cr0_mask);
        vmx_vmcs_write(VMCS_CR0_READ_SHADOW, cr0);
        vmx_vmcs_write(VMCS_CR4_GUEST_HOST_MASK, cr4_fixed0);
        vmx_vmcs_write(VMCS_CR4_READ_SHADOW, cr4 & ~cr4_fixed0);
        log_hex64("[LINUX] CR0 mask=", cr0_mask);
        log_hex64("[LINUX] CR4 mask=", cr4_fixed0);
    }

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

    /* Access rights.
     * P8.5: selector=0 的段必须标记 unusable（access rights bit 16），
     * 否则 VM-entry guest-state 检查失败：
     *   FS/GS selector=0 → 0x1C093（unusable）
     *   LDTR selector=0 → 0x10082（unusable，旧值 0x82 缺 bit 16） */
    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);  /* 64-bit code, L=1 */
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0x1C093);  /* unusable (selector=0) */
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0x1C093);  /* unusable (selector=0) */
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x10082); /* unusable (selector=0) */
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
    /* SCE must stay set across VM-exits or userspace SYSCALL #UDs
     * (busybox musl arch_prctl). Combined with VM_EXIT_SAVE_GUEST_EFER. */
    vmx_vmcs_write(VMCS_GUEST_IA32_EFER,
                   EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE);

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
    log_hex64("[LINUX] TRUE_PINBASED_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_PINBASED_CTLS));
    log_hex64("[LINUX] TRUE_PROCBASED_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_PROCBASED_CTLS));
    log_hex64("[LINUX] PROCBASED_CTLS2 raw=",
              vmx_read_msr(IA32_VMX_PROCBASED_CTLS2));
    log_hex64("[LINUX] TRUE_EXIT_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_EXIT_CTLS));
    log_hex64("[LINUX] TRUE_ENTRY_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_ENTRY_CTLS));

    /* Pin-based：外部中断 exit + NMI exit + VMX preemption timer（若支持）。
     * preemption timer 提供 1ms 周期 exit，用于轮询 host 串口 RX、
     * 推进 guest PIT tick、注入 pending virtio/COM1 IRQ。 */
    u64 pin_want = PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING |
                   PIN_VIRTUAL_NMIS;
    if (vmx_preemption_timer_supported()) {
        pin_want |= PIN_VMX_PREEMPTION_TIMER;
    }
    u64 pin = vmx_adjust_control(pin_want, IA32_VMX_TRUE_PINBASED_CTLS);

    /* CPU-based: exit on HLT, INVLPG, MWAIT, IO, use MSR bitmap,
     * activate secondary controls. Don't intercept RDTSC (Linux needs it). */
    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING
            | CPU_BASED_CR3_LOAD_EXITING
            | CPU_BASED_CR3_STORE_EXITING;
    cpu = vmx_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    /* Secondary: EPT + VPID（经 capability MSR 调整；
     * EPT 不可用则打日志——Linux guest 的 GPA 布局依赖 EPT）。 */
    u64 cpu2 = SEC_EXEC_ENABLE_EPT | SEC_EXEC_ENABLE_VPID |
               SEC_EXEC_ENABLE_RDTSCP;
    cpu2 = vmx_adjust_control(cpu2, IA32_VMX_PROCBASED_CTLS2);
    if (!(cpu2 & SEC_EXEC_ENABLE_EPT)) {
        log_warn("[LINUX] EPT not available (shadow paging) - guest GPA map disabled");
    }

    u64 exit_ctrl = VM_EXIT_SAVE_DEBUG_CONTROLS
                  | VM_EXIT_HOST_ADDR_SPACE_SIZE
                  | VM_EXIT_SAVE_GUEST_EFER
                  | VM_EXIT_LOAD_HOST_EFER
                  | VM_EXIT_SAVE_CET_STATE
                  | VM_EXIT_LOAD_CET_STATE;
    exit_ctrl = vmx_adjust_control(exit_ctrl, IA32_VMX_TRUE_EXIT_CTLS);

    u64 entry_ctrl = VM_ENTRY_LOAD_DEBUG_CONTROLS
                   | VM_ENTRY_IA32E_MODE_GUEST
                   | VM_ENTRY_LOAD_GUEST_EFER
                   | VM_ENTRY_LOAD_CET_STATE;
    entry_ctrl = vmx_adjust_control(entry_ctrl, IA32_VMX_TRUE_ENTRY_CTLS);

    g_linux_cet_vmcs = 0;
    if ((exit_ctrl & VM_EXIT_SAVE_CET_STATE) &&
        (exit_ctrl & VM_EXIT_LOAD_CET_STATE) &&
        (entry_ctrl & VM_ENTRY_LOAD_CET_STATE)) {
        g_linux_cet_vmcs = 1;
        log_info("[LINUX] VMCS CET state switch enabled");
    } else {
        exit_ctrl &= ~(VM_EXIT_SAVE_CET_STATE | VM_EXIT_LOAD_CET_STATE);
        entry_ctrl &= ~VM_ENTRY_LOAD_CET_STATE;
        log_warn("[LINUX] VMCS CET switch not offered by nested VMX");
    }

    vmx_vmcs_write(VMCS_PIN_BASED_VM_EXEC_CONTROL, pin);
    vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL, cpu);
    vmx_vmcs_write(VMCS_SECONDARY_VM_EXEC_CONTROL, cpu2);
    vmx_vmcs_write(VMCS_VM_EXIT_CONTROLS, exit_ctrl);
    vmx_vmcs_write(VMCS_VM_ENTRY_CONTROLS, entry_ctrl);

    /* MSR bitmap: intercept EFER (SCE must land in VMCS) and FS/GS bases
     * (arch_prctl). Nested KVM does not apply un-intercepted WRMSR
     * IA32_FS_BASE to GUEST_FS_BASE; ignoring those writes leaves TLS=0
     * and every libc/musl process SIGSEGVs at address 0. */
    {
        u32 efer_off = 0xC0000080u - 0xC0000000u;
        u32 fs_off   = 0xC0000100u - 0xC0000000u;
        u32 gs_off   = 0xC0000101u - 0xC0000000u;
        u32 kgs_off  = 0xC0000102u - 0xC0000000u;
        g_linux_msr_bitmap[0x400 + efer_off / 8] |= (u8)(1u << (efer_off & 7));
        g_linux_msr_bitmap[0xC00 + efer_off / 8] |= (u8)(1u << (efer_off & 7));
        g_linux_msr_bitmap[0x400 + fs_off / 8]   |= (u8)(1u << (fs_off & 7));
        g_linux_msr_bitmap[0xC00 + fs_off / 8]   |= (u8)(1u << (fs_off & 7));
        g_linux_msr_bitmap[0x400 + gs_off / 8]   |= (u8)(1u << (gs_off & 7));
        g_linux_msr_bitmap[0xC00 + gs_off / 8]   |= (u8)(1u << (gs_off & 7));
        g_linux_msr_bitmap[0xC00 + kgs_off / 8]  |= (u8)(1u << (kgs_off & 7));
        g_linux_msr_bitmap[0x400 + kgs_off / 8]  |= (u8)(1u << (kgs_off & 7));
        /* Do not intercept CET MSRs: L1 CR4.CET=0 so we cannot WRMSR
         * them, but L2 hardware CET is on. The guest driver clears
         * IA32_U_CET with a passthrough WRMSR. */
        u64 msr_bitmap_phys = vmx_kernel_virt_to_phys(g_linux_msr_bitmap);
        log_hex64("[LINUX] MSR bitmap phys=", msr_bitmap_phys);
        vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);
    }

    /* Nested KVM does not offer VMCS CET save/load, and it also hides
     * CET from L1 CPUID/CR4. L0 U_CET still leaks into L2 (busybox PUSH
     * to SSP=0). Always try a VM-entry MSR-load of IA32_U_CET=0; if
     * nested rejects the list, linux_launch retries with count=0. */
    {
        u64 host_cr4 = read_cr4_local();
        log_hex64("[LINUX] host CR4=", host_cr4);
        g_host_ucet = 0;
        g_linux_cet_force = 0;
        if (host_cr4 & CR4_CET) {
            g_host_ucet = vmx_read_msr(0x6A0);
            g_linux_cet_force = 1;
            log_hex64("[LINUX] host U_CET=", g_host_ucet);
            log_info("[LINUX] CR4.CET set: WRMSR U_CET=0 on each L2 entry");
        } else {
            log_warn("[LINUX] CR4.CET=0: cannot WRMSR CET; MSR-load list only");
        }

        g_linux_msr_entry_load[0].index = 0x6A0;
        g_linux_msr_entry_load[0].reserved = 0;
        g_linux_msr_entry_load[0].data = 0;
        /* Nested KVM reports EXIT_ENTRY_FAIL_MSR_LOADING (34) for CET
         * MSRs. Do not arm the list; QEMU-process arch_prctl is the
         * working way to keep leaked U_CET off. */
        g_linux_msr_entry_count = 0;
        vmx_vmcs_write(VMCS_VM_ENTRY_MSR_LOAD_COUNT, 0);
        vmx_vmcs_write(VMCS_VM_EXIT_MSR_LOAD_COUNT, 0);
        vmx_vmcs_write(VMCS_VM_EXIT_MSR_STORE_COUNT, 0);
        log_info("[LINUX] CET MSR-load list not armed (nested rejects U_CET)");
    }

    if (cpu2 & SEC_EXEC_ENABLE_EPT) {
        vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    } else {
        vmx_vmcs_write(VMCS_EPT_POINTER, 0);
    }
    if (cpu2 & SEC_EXEC_ENABLE_VPID) {
        vmx_vmcs_write(VMCS_VPID, 2);  /* VPID 1 = self-test, VPID 2 = Linux */
    } else {
        vmx_vmcs_write(VMCS_VPID, 0);
    }

    /* Linux owns #PF while it grows the identity map. Intercepting it
     * plus serial dumps livelocks extract_kernel. Catch #UD/#GP/#DF only. */
    /* #UD #DF #GP #CP(CET). User #GP currently does not nested-exit;
     * still catch #CP if L0 delivers it to L1. */
    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP,
                   (1ULL << 6) | (1ULL << 8) | (1ULL << 13) | (1ULL << 21));
}

/* ===== Global for passing RSI to launch asm ===== */
static volatile u64 g_linux_rsi;
static volatile int g_linux_launch_failed;
static volatile u64 g_linux_launch_flags;

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
    linux_xsave_init();

    /* 注册 virtio-mmio 后端（block + net），guest 经 cmdline
     * virtio_mmio.device= 发现。幂等：重复调用直接返回。 */
    virtio_mmio_init();

    /* 真机 EPT TLB 刷新：在 vmlaunch 前确保 EPT 映射对 TLB 可见 */
    ept_flush_ept();

    /* Reset VM-Exit state */
    g_guest_terminated = 0;
    g_last_exit_reason = 0xFFFFFFFFULL;
    g_linux_guest_active = 1;   /* 标记当前 guest 为 Linux（HLT 时 park） */
    g_guest_parked = 0;

    /* Configure VMCS */
    linux_vmcs_setup_guest_state(gi);
    linux_vmcs_setup_controls(vmm_get_eptp());
    linux_vmcs_setup_host_state();

    /* 仅在 pin-based control 真正启用 timer 时 arm。 */
    if (vmx_vmcs_read(VMCS_PIN_BASED_VM_EXEC_CONTROL) &
        PIN_VMX_PREEMPTION_TIMER) {
        vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER,
                       vmx_preemption_quantum());
    } else {
        vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, 0);
    }

    /* Set RSI for Linux boot_params (passed via global, loaded in asm) */
    g_linux_rsi = gi->bootparams_gpa;

    /* P8.5: VMCLEAR + VMPTRLD —— VMLAUNCH 要求 VMCS 处于 "clear" 态。
     * 自检成功后 VMCS 状态为 "launched"，不 VMCLEAR 会报
     * "VMLAUNCH with non-clear VMCS"（error 4）。
     * 内容不受影响：VMCLEAR 只回写内存并改状态，VMPTRLD 再挂载。 */
    if (vmx_vmcs_clear(vmm_get_vmcs_phys()) != 0) {
        log_error("[LINUX] vmclear failed");
        return -4;
    }
    if (vmx_vmcs_load(vmm_get_vmcs_phys()) != 0) {
        log_error("[LINUX] vmptrld after vmclear failed");
        return -5;
    }

    log_hex64("[LINUX] RIP=", gi->kernel_entry);
    log_hex64("[LINUX] RSP=", LINUX_GUEST_STACK_GPA + 0x1000);
    log_hex64("[LINUX] RSI=", gi->bootparams_gpa);
    log_hex64("[LINUX] CR3=", gi->pgt_gpa);
    log_hex64("[LINUX] GDTR=", gi->gdt_gpa);
    log_hex64("[LINUX] pin=", vmx_vmcs_read(VMCS_PIN_BASED_VM_EXEC_CONTROL));
    log_hex64("[LINUX] cpu=", vmx_vmcs_read(VMCS_CPU_BASED_VM_EXEC_CONTROL));
    log_hex64("[LINUX] cpu2=", vmx_vmcs_read(VMCS_SECONDARY_VM_EXEC_CONTROL));
    log_hex64("[LINUX] exit=", vmx_vmcs_read(VMCS_VM_EXIT_CONTROLS));
    log_hex64("[LINUX] entry=", vmx_vmcs_read(VMCS_VM_ENTRY_CONTROLS));
    log_hex64("[LINUX] eptp=", vmx_vmcs_read(VMCS_EPT_POINTER));
    log_hex64("[LINUX] msr_bitmap=", vmx_vmcs_read(VMCS_MSR_BITMAP));
    log_hex64("[LINUX] vpid=", vmx_vmcs_read(VMCS_VPID));
    linux_cet_vmcs_sync_host();
    linux_cet_force_guest_off();
    log_info("[LINUX] vmlaunch");
    linux_xsave_load_guest();

    /* vmlaunch with RSI = boot_params.
     * Same control flow pattern as vmm_self_test:
     *   - On failure: failed=1, falls through
     *   - On success: enters guest, VM-Exit → vmx_vm_exit_handler
     *   - On terminate: handler restores RSP, jmps to post_guest label */
    g_linux_launch_failed = 1;
    g_linux_launch_flags = 0;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"
        /* Linux 64-bit boot protocol requires every GPR except RSI zero. */
        "xorl %%eax, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%edi, %%edi\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "xorl %%r8d, %%r8d\n\t"
        "xorl %%r9d, %%r9d\n\t"
        "xorl %%r10d, %%r10d\n\t"
        "xorl %%r11d, %%r11d\n\t"
        "xorl %%r12d, %%r12d\n\t"
        "xorl %%r13d, %%r13d\n\t"
        "xorl %%r14d, %%r14d\n\t"
        "xorl %%r15d, %%r15d\n\t"
        "movq g_linux_rsi(%%rip), %%rsi\n\t"   /* RSI = boot_params GPA */
        "vmlaunch\n\t"
        /* ---- failure path ---- */
        "movl $1, g_linux_launch_failed(%%rip)\n\t"
        "pushfq\n\t"
        "popq g_linux_launch_flags(%%rip)\n\t"
        "jmp 2f\n\t"
        /* ---- post_guest: terminate path jmps here ---- */
        "1:\n\t"
        "movl $0, g_linux_launch_failed(%%rip)\n\t"
        "2:\n\t"
        :
        :
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "cc", "memory"
    );

    int failed = g_linux_launch_failed;
    u64 vm_flags = g_linux_launch_flags;
    if (failed && g_linux_msr_entry_count) {
        u64 error = 0;
        if (vm_flags & 0x40)
            vmx_vmcs_read_checked(VMCS_VMX_INSTRUCTION_ERROR, &error);
        log_warn("[LINUX] vmlaunch failed with CET MSR-load; retry without");
        log_hex64("[LINUX] vmlaunch CET-load error=", error);
        vmx_vmcs_write(VMCS_VM_ENTRY_MSR_LOAD_COUNT, 0);
        g_linux_msr_entry_count = 0;
        g_linux_launch_failed = 1;
        g_linux_launch_flags = 0;
        linux_cet_force_guest_off();
        __asm__ volatile(
            "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"
            "leaq 1f(%%rip), %%rax\n\t"
            "movq %%rax, g_saved_return_rip(%%rip)\n\t"
            "xorl %%eax, %%eax\n\t"
            "xorl %%ebx, %%ebx\n\t"
            "xorl %%ecx, %%ecx\n\t"
            "xorl %%edx, %%edx\n\t"
            "xorl %%edi, %%edi\n\t"
            "xorl %%ebp, %%ebp\n\t"
            "xorl %%r8d, %%r8d\n\t"
            "xorl %%r9d, %%r9d\n\t"
            "xorl %%r10d, %%r10d\n\t"
            "xorl %%r11d, %%r11d\n\t"
            "xorl %%r12d, %%r12d\n\t"
            "xorl %%r13d, %%r13d\n\t"
            "xorl %%r14d, %%r14d\n\t"
            "xorl %%r15d, %%r15d\n\t"
            "movq g_linux_rsi(%%rip), %%rsi\n\t"
            "vmlaunch\n\t"
            "movl $1, g_linux_launch_failed(%%rip)\n\t"
            "pushfq\n\t"
            "popq g_linux_launch_flags(%%rip)\n\t"
            "jmp 2f\n\t"
            "1:\n\t"
            "movl $0, g_linux_launch_failed(%%rip)\n\t"
            "2:\n\t"
            :
            :
            : "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp",
              "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
              "cc", "memory"
        );
        failed = g_linux_launch_failed;
        vm_flags = g_linux_launch_flags;
    }
    linux_cet_restore_host();
    linux_xsave_save_guest();
    linux_xsave_load_host();
    if (failed) {
        /* CF=1 → VMfailInvalid；ZF=1 → VMfailValid（error field 有效）。 */
        if (vm_flags & 0x1) {
            log_error("[LINUX] vmlaunch VMfailInvalid (invalid current-VMCS)");
        } else if (vm_flags & 0x40) {
            log_error("[LINUX] vmlaunch VMfailValid");
        }
        u64 error = 0;
        if (vm_flags & 0x40) {
            vmx_vmcs_read_checked(VMCS_VMX_INSTRUCTION_ERROR, &error);
        }
        log_hex64("[LINUX] vmlaunch failed, error=", error);
        g_linux_guest_active = 0;
        return -3;
    }

    /* Only handle_hlt() may mark the Linux guest parked.  A fatal exception,
     * triple fault, or EPT failure also returns through post_guest, but must
     * not make the compatibility layer report a ready daemon. */
    g_linux_guest_active = 0;
    log_hex64("[LINUX] last exit reason=", g_last_exit_reason);
    log_hex64("[LINUX] vmexit count=", vmexit_get_count());
    if (!g_guest_parked) {
        log_error("[LINUX] guest terminated before daemon park");
        return -6;
    }

    /* A kernel idle/fatal HLT is indistinguishable from the daemon's PARK at
     * the VM-exit reason level.  The daemon must first publish EXEC_READY
     * through /dev/utsm; this proves the driver PING, SHM mapping, userspace
     * daemon, and Linux→UTSM ring are all alive. */
    {
        struct utsm_ipc_msg ready;
        int got_ready = 0;
        for (int i = 0; i < 8; i++) {
            if (ipc_shm_recv(&ready) != 0) break;
            if (ready.type == UTSM_MSG_EXEC_READY) {
                got_ready = 1;
                break;
            }
            log_hex64("[LINUX] pre-ready message type=", ready.type);
        }
        if (!got_ready) {
            g_guest_parked = 0;
            log_error("[LINUX] HLT without EXEC_READY handshake");
            return -7;
        }
    }

    log_info("[LINUX] EXEC_READY received from /dev/utsm");
    log_info("[LINUX] guest parked (daemon ready)");

    return 0;
}
