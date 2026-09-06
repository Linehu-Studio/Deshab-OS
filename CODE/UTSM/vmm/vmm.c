#include <utsm/vmm.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== VMX 指令错误码（Intel SDM Vol 3D, "VM Instruction Error Numbers"） ===== */
static const char *vmx_error_str(u64 err) {
    switch (err) {
    case 1: return "VMCALL executed in VMX root operation";
    case 2: return "VMCLEAR with invalid physical address";
    case 3: return "VMCLEAR with VMXON pointer";
    case 4: return "VMLAUNCH with non-clear VMCS";
    case 5: return "VMRESUME with non-launched VMCS";
    case 6: return "VMRESUME after VMXOFF";
    case 7: return "VM entry with invalid VM-execution control fields";
    case 8: return "VM entry with invalid host-state fields";
    case 9: return "VMPTRLD with invalid physical address";
    case 10: return "VMPTRLD with VMXON pointer";
    case 11: return "VMPTRLD with incorrect VMCS revision identifier";
    case 12: return "VMREAD from unsupported VMCS component";
    case 13: return "VMWRITE to read-only VMCS component";
    case 15: return "VM entry with invalid guest-state fields";
    case 16: return "VM entry with invalid executive-VMCS pointer";
    case 17: return "VM entry with non-launched executive VMCS";
    case 18: return "VM entry with executive-VMCS inconsistency";
    case 19: return "VM entry with invalid VM-exit control fields";
    case 20: return "VM entry with invalid MSEG VMCS revision";
    case 22: return "VMCALL with non-clear VMCS";
    case 23: return "VMCALL with invalid VM-exit control fields";
    case 24: return "VMCALL with incorrect MSEG VMCS revision";
    case 25: return "VM switch with invalid VM-exit control fields";
    case 26: return "VM switch with invalid VM-exit MSEG VMCS revision";
    case 28: return "VMRETURN with non-launched VMCS";
    case 29: return "VMRETURN with invalid executive VMCS";
    case 30: return "VM entry with invalid VM-execution control fields in event injection";
    default: return "unknown";
    }
}

/* ===== VMM 全局状态 ===== */
static int g_vmm_ready;
static u64 g_vmcs_phys;

/* VM-Exit 后回到 host 的栈与 RIP 恢复点。
 * vmm_self_test 在 vmlaunch 前保存当前 RSP 与"launch 后"的 RIP，
 * VM-Exit 处理器在决定终止 guest 时恢复 RSP 并 jmp 到该 RIP。
 * 注意：这些变量必须是非 static 的全局符号，因为 vmexit_asm.S 通过
 * RIP 相对寻址引用它们。 */
volatile u64 g_saved_host_rsp;
volatile u64 g_saved_return_rip;
volatile int g_guest_terminated;
volatile u64 g_last_exit_reason;

/* Linux guest park-and-resume 状态。
 * g_linux_guest_active: 1 = 当前运行的 guest 是 Linux（HLT 时 park，不终止）
 * g_guest_parked: 1 = Linux guest 已 HLT 驻留，等待 vmresume 唤醒 */
volatile int g_linux_guest_active;
volatile int g_guest_parked;

/* Guest GPR save area (referenced by vmexit_asm.S).
 * VM-Exit 时 CPU 不自动保存 guest GPR，由汇编入口保存到此结构。
 * C 处理器可读写此结构（如 VMCALL 返回值写入 rax）。 */
volatile struct vmexit_guest_regs g_guest_regs;

/* host 栈：VM-Exit 时使用（HOST_RSP）。
 * 非static：linux_boot.c 共用同一个 host 栈。 */
u8 g_host_stack[8192] __attribute__((aligned(16)));

/* VM-Exit 汇编入口（vmexit_asm.S） */
extern void vmx_vm_exit_handler(void);

/* C 分发器（vmexit.c） */
extern int vmexit_dispatch(void);
extern u64 vmexit_get_count(void);

/* ===== Phase 1.1 自检 guest 代码 =====
 * guest 在 GPA 0x1000 运行，执行 nop;nop;nop;hlt。
 * HLT 触发 VMEXIT，dispatch 返回 0（终止）。 */
static u8 g_guest_code[4096] __attribute__((aligned(4096))) = {
    0x90, 0x90, 0x90, 0xF4,   /* nop; nop; nop; hlt */
};

/* guest 栈页 */
static u8 g_guest_stack[4096] __attribute__((aligned(4096)));

/* P8.5: guest 自有页表（EPT 模式下 guest CR3 不能指向 host 页表——
 * host 页表页未在 EPT 映射，页表遍历本身就会 EPT violation）。
 * 布局（GPA）：
 *   0x6000 PML4  [0] -> 0x7000 | P|RW
 *   0x7000 PDPT  [0] -> 0x9000 | P|RW
 *   0x9000 PD    [0] -> 2MB 大页 | P|RW|PS  （GPA 0..2MB 恒等映射）
 * guest 代码/栈/GDT/TSS/IDT 页全部落在低 2MB 内。 */
static u64 g_guest_pml4[512] __attribute__((aligned(4096)));
static u64 g_guest_pdpt[512] __attribute__((aligned(4096)));
static u64 g_guest_pd[512] __attribute__((aligned(4096)));

/* MSR bitmap（4KB 全 0 = 不拦截 MSR） */
static u8 g_msr_bitmap[4096] __attribute__((aligned(4096)));

/* guest GDT（GPA 0x3000）与 TSS（GPA 0x4000）。
 * VMX 要求 guest TR 非空且引用 GDT 中的 TSS 描述符。 */
static u8 g_guest_gdt[4096] __attribute__((aligned(4096)));
static u8 g_guest_tss[4096] __attribute__((aligned(4096)));
static u8 g_guest_idt[4096] __attribute__((aligned(4096)));  /* P8.4: IDT page for KVM nested VMX */

/* 64-bit GDT entry */
typedef struct __attribute__((packed)) {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
} gdt_entry64;

/* 64-bit TSS descriptor (16 bytes) */
typedef struct __attribute__((packed)) {
    u32 limit_low_base_low;
    u8  base_mid;
    u8  access;
    u8  flags_limit_high;
    u8  base_high;
    u32 base_upper;
    u32 reserved;
} tss_desc64;

static void build_guest_gdt(void) {
    /* 清零 */
    for (int i = 0; i < 4096; i++) g_guest_gdt[i] = 0;
    for (int i = 0; i < 4096; i++) g_guest_tss[i] = 0;

    gdt_entry64 *gdt = (gdt_entry64 *)g_guest_gdt;
    /* [0x00] null */
    /* [0x08] code: base=0, limit=0xFFFFF, L=1, G=1, present, DPL0, type=0x9B */
    gdt[1].limit_low = 0xFFFF;
    gdt[1].base_low = 0;
    gdt[1].base_mid = 0;
    gdt[1].access = 0x9B;       /* present, DPL0, code, executed/read */
    gdt[1].flags_limit_high = 0xAF;  /* G=1, L=1 (64-bit), limit[19:16]=0xF */
    gdt[1].base_high = 0;
    /* [0x10] data: base=0, limit=0xFFFFF, G=1, present, DPL0, type=0x93 */
    gdt[2].limit_low = 0xFFFF;
    gdt[2].base_low = 0;
    gdt[2].base_mid = 0;
    gdt[2].access = 0x93;       /* present, DPL0, data, read/write */
    gdt[2].flags_limit_high = 0xCF;  /* G=1, DB=1, limit[19:16]=0xF */
    gdt[2].base_high = 0;

    /* [0x18] TSS descriptor (16 bytes, 64-bit TSS) */
    tss_desc64 *tss_d = (tss_desc64 *)&g_guest_gdt[0x18];
    u64 tss_base = 0x4000;      /* TSS at GPA 0x4000 */
    u32 tss_limit = 0x67;       /* 104-byte minimum TSS */
    tss_d->limit_low_base_low = (tss_limit & 0xFFFF) | ((u32)(tss_base & 0xFFFFFF) << 16);
    tss_d->base_mid = (u8)((tss_base >> 24) & 0xFF);
    tss_d->access = 0x89;       /* present, DPL0, 64-bit TSS */
    tss_d->flags_limit_high = (u8)((tss_limit >> 16) & 0x0F) | 0x00;
    tss_d->base_high = 0;
    tss_d->base_upper = 0;
    tss_d->reserved = 0;
}

/* ===== CR 读取辅助 ===== */
static inline u64 read_cr0_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr4_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}
static inline u64 read_cr3_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* ===== VMCS 配置 ===== */

static void vmcs_setup_host_state(void) {
    vmx_vmcs_write(VMCS_HOST_CR0, read_cr0_local() | CR0_PE | CR0_NE | CR0_PG);
    vmx_vmcs_write(VMCS_HOST_CR3, read_cr3_local());
    vmx_vmcs_write(VMCS_HOST_CR4, read_cr4_local() | CR4_VMXE);

    /* 段选择子：UTSM GDT 0x08 (code) / 0x10 (data) */
    vmx_vmcs_write(VMCS_HOST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_HOST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_HOST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_HOST_GS_SELECTOR, 0);

    vmx_vmcs_write(VMCS_HOST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_HOST_GS_BASE, 0);

    /* Host GDT/IDT base + TR（VM-Exit 后 CPU 加载 host TR，必须有效） */
    {
        struct __attribute__((packed)) { u16 limit; u64 base; } gdtr, idtr;
        __asm__ volatile("sgdt %0" : "=m"(gdtr));
        __asm__ volatile("sidt %0" : "=m"(idtr));
        vmx_vmcs_write(VMCS_HOST_GDTR_BASE, gdtr.base);
        vmx_vmcs_write(VMCS_HOST_IDTR_BASE, idtr.base);

        /* 读取当前 TR 寄存器（UTSM 已加载 TSS） */
        u16 tr_sel;
        u64 tr_base;
        __asm__ volatile("str %0" : "=r"(tr_sel));
        /* 从 GDT 中读取 TR base：TR 选择子指向的描述符 */
        u64 gdt_base = gdtr.base;
        u64 *tss_desc = (u64 *)(gdt_base + (tr_sel & 0xFFF8));
        /* 64-bit TSS 描述符 16 字节：base 由 bits 拼接 */
        u64 low = tss_desc[0];
        u64 high = tss_desc[1];
        tr_base = (low >> 16) & 0xFFFFFF;          /* base[15:0] + base[23:16] */
        tr_base |= ((low >> 32) & 0xFF) << 24;      /* base[31:24] */
        tr_base |= (high & 0xFFFFFFFF) << 32;       /* base[63:32] */
        vmx_vmcs_write(VMCS_HOST_TR_SELECTOR, tr_sel);
        vmx_vmcs_write(VMCS_HOST_TR_BASE, tr_base);
    }

    /* Host SYSENTER（VMX 要求字段有效） */
    {
        u64 sysenter_cs;
        __asm__ volatile("rdmsr" : "=a"(sysenter_cs) : "c"(0x174));
        vmx_vmcs_write(VMCS_HOST_SYSENTER_CS, sysenter_cs & 0xFFFFFFFF);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_ESP, 0);
        vmx_vmcs_write(VMCS_HOST_SYSENTER_EIP, 0);
    }

    /* Host EFER：VM_EXIT_LOAD_HOST_EFER 控制位要求 VMCS 中有 host EFER 值，
     * VM-Exit 时加载。读取当前 host EFER MSR 填入。 */
    {
        u32 low, high;
        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
        u64 host_efer = ((u64)high << 32) | low;
        vmx_vmcs_write(VMCS_HOST_IA32_EFER, host_efer);
    }

    /* Host RSP/RIP：VM-Exit 后跳到 vmx_vm_exit_handler */
    vmx_vmcs_write(VMCS_HOST_RSP, (u64)&g_host_stack[sizeof(g_host_stack) - 16]);
    vmx_vmcs_write(VMCS_HOST_RIP, (u64)vmx_vm_exit_handler);
}

static void vmcs_setup_guest_state_phase1(void) {
    /* Guest CR0/CR3/CR4：启用 PE/NE/PG/PAE。
     * P8.5: CR3 指向 guest 自有页表（GPA 0x6000），
     * 不再借用 host CR3（EPT 下 host 页表页未映射，遍历会失败）。 */
    vmx_vmcs_write(VMCS_GUEST_CR0, CR0_PE | CR0_NE | CR0_PG | CR0_WP);
    vmx_vmcs_write(VMCS_GUEST_CR3, 0x6000);
    vmx_vmcs_write(VMCS_GUEST_CR4, CR4_VMXE | CR4_PAE | CR4_PGE | CR4_PSE);

    /* 段选择子：flat 模型。
     * CS=0x08 (GDT[1] code), DS/SS/ES=0x10 (GDT[2] data),
     * TR=0x18 (GDT[3] 64-bit TSS)。 */
    vmx_vmcs_write(VMCS_GUEST_CS_SELECTOR, 0x08);
    vmx_vmcs_write(VMCS_GUEST_SS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_DS_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_ES_SELECTOR, 0x10);
    vmx_vmcs_write(VMCS_GUEST_FS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_SELECTOR, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_SELECTOR, 0x18);

    /* 段 base：flat 模型全 0，TR base 指向 GPA 0x4000 的 TSS */
    vmx_vmcs_write(VMCS_GUEST_CS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_SS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_DS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_ES_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_FS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_GS_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_LDTR_BASE, 0);
    vmx_vmcs_write(VMCS_GUEST_TR_BASE, 0x4000);
    vmx_vmcs_write(VMCS_GUEST_GDTR_BASE, 0x3000);
    /* P8.4: IDTR base must be non-zero & page-aligned for KVM nested VMX.
     * Guest won't use interrupts (RFLAGS.IF=0), but VMX still checks. */
    vmx_vmcs_write(VMCS_GUEST_IDTR_BASE, 0x5000);

    /* 段 limit：代码/数据段 4GB，GDTR 覆盖 0x28 字节（3 个描述符 + TSS），
     * TR limit = 0x67（104 字节最小 TSS） */
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

    /* 段 access rights：
     * CS: 0xA09B = present, DPL0, code, L=1 (64-bit), executed/read
     * DS/SS/ES: 0xC093 = present, DPL0, data, writable
     * P8.4: FS/GS/LDTR selector=0 -> must set bit 16 (unusable) per Intel SDM.
     *   Without it, KVM nested VMX rejects VMLAUNCH (invalid guest state).
     * TR: 0x8B = present, DPL0, 64-bit busy TSS */
    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0x1C093);   /* unusable (selector=0) */
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0x1C093);   /* unusable (selector=0) */
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x10082); /* unusable (selector=0) */
    vmx_vmcs_write(VMCS_GUEST_TR_ACCESS, 0x8B);

    /* Guest RIP/RSP/RFLAGS */
    vmx_vmcs_write(VMCS_GUEST_RIP, 0x1000);
    vmx_vmcs_write(VMCS_GUEST_RSP, 0x8000);
    vmx_vmcs_write(VMCS_GUEST_RFLAGS, 0x2);

    /* Guest SYSENTER（VMX 要求字段有效，即使 guest 不使用） */
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_vmcs_write(VMCS_GUEST_SYSENTER_EIP, 0);

    /* Guest DR7 = 默认值 */
    vmx_vmcs_write(VMCS_GUEST_DR7, 0x400);

    /* Guest EFER：64-bit guest (CS.L=1) 要求 EFER.LME=1 且 EFER.LMA=1，
     * 否则 VMX guest-state 检查失败（error 8: invalid guest state）。
     * NXE 允许 guest 使用 NX 位。 */
    vmx_vmcs_write(VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA | EFER_NXE);

    /* P8.4: VMCS link pointer must be 0xFFFFFFFFFFFFFFFF (Intel SDM Vol 3,
     * VM-entry checks). KVM nested VMX enforces this strictly. */
    vmx_vmcs_write(VMCS_GUEST_LINK_POINTER, 0xFFFFFFFFFFFFFFFFULL);

    /* Guest activity state = 0 (active) */
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    /* Guest interruptibility = 0（无阻塞） */
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

static void vmcs_setup_controls(u64 eptp) {
    log_hex64("[VMM] TRUE_PINBASED_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_PINBASED_CTLS));
    log_hex64("[VMM] TRUE_PROCBASED_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_PROCBASED_CTLS));
    log_hex64("[VMM] PROCBASED_CTLS2 raw=",
              vmx_read_msr(IA32_VMX_PROCBASED_CTLS2));
    log_hex64("[VMM] TRUE_EXIT_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_EXIT_CTLS));
    log_hex64("[VMM] TRUE_ENTRY_CTLS raw=",
              vmx_read_msr(IA32_VMX_TRUE_ENTRY_CTLS));

    u64 pin = vmx_adjust_control(PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING |
                                 PIN_VIRTUAL_NMIS,
                                 IA32_VMX_TRUE_PINBASED_CTLS);

    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING;
    cpu = vmx_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    /* P8.5: 请求 EPT（KVM 报告的 allowed-1 集合含 EPT 位）。
     * 自检 guest 用 GPA 0x1000..0x9000 + 自有页表，必须开 EPT
     * 才能让 ept_map_range 的映射真正生效。
     * EPT 不可用时 cpu2 会失去 EPT 位并打日志，自检将失败。 */
    u64 cpu2 = SEC_EXEC_ENABLE_EPT;
    cpu2 = vmx_adjust_control(cpu2, IA32_VMX_PROCBASED_CTLS2);

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

    /* MSR bitmap 物理地址 */
    {
        u64 msr_bitmap_phys = vmx_kernel_virt_to_phys(g_msr_bitmap);
        log_hex64("[VMM] MSR bitmap phys=", msr_bitmap_phys);
        vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);
    }

    if (cpu2 & SEC_EXEC_ENABLE_EPT) {
        vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
        log_hex64("[VMM] EPT enabled, eptp=", eptp);
    } else {
        log_info("[VMM] EPT not available (shadow paging)");
    }
    if (cpu2 & SEC_EXEC_ENABLE_VPID) {
        vmx_vmcs_write(VMCS_VPID, 1);
    } else {
        vmx_vmcs_write(VMCS_VPID, 0);
    }

    /* Exception bitmap：捕获 #GP(13)/#PF(14)/#UD(6) */
    vmx_vmcs_write(VMCS_EXCEPTION_BITMAP, (1ULL << 13) | (1ULL << 14) | (1ULL << 6));
}

/* ===== VMM API ===== */

int vmm_init(void) {
    log_info("[VMM] init begin");

    if (vmx_enable() != 0) {
        log_error("[VMM] VMX enable failed");
        return -1;
    }

    if (ept_init() != 0) {
        log_error("[VMM] EPT init failed");
        return -2;
    }

    if (vmx_vmcs_alloc(&g_vmcs_phys) != 0) {
        log_error("[VMM] VMCS alloc failed");
        return -3;
    }
    log_hex64("[VMM] VMCS phys=", g_vmcs_phys);

    if (vmx_vmcs_load(g_vmcs_phys) != 0) {
        log_error("[VMM] VMCS load failed");
        return -4;
    }

    /* VMCS 字段读写自检 */
    vmx_vmcs_write(VMCS_VPID, 0x1234);
    u64 vpid_readback = vmx_vmcs_read(VMCS_VPID);
    if (vpid_readback != 0x1234) {
        log_error("[VMM] VMCS read/write verify failed");
        log_hex64("[VMM] wrote=0x1234 read=", vpid_readback);
        return -5;
    }
    vmx_vmcs_write(VMCS_VPID, 1);

    g_vmm_ready = 1;
    log_info("[VMM] init ok");

    /* 真机支持检查：INVVPID/INVEPT */
    ept_check_vpid_support();

    return 0;
}

void vmm_shutdown(void) {
    if (!g_vmm_ready) return;
    vmx_disable();
    g_vmm_ready = 0;
}

int vmm_is_ready(void) { return g_vmm_ready; }
u64 vmm_get_eptp(void) { return ept_get_eptp(); }
/* P8.5: 暴露 VMCS 物理地址，供 linux_launch() 在 vmlaunch 前
 * 执行 VMCLEAR+VMPTRLD（VMCS 状态机：VMLAUNCH 要求 clear 态）。 */
u64 vmm_get_vmcs_phys(void) { return g_vmcs_phys; }

/* P8.4: Dump all guest-state VMCS fields for KVM nested VMX debugging.
 * Called before VMLAUNCH to identify field encoding/value errors. */
static void vmm_dump_guest_state(void) {
    log_info("[VMM] === Guest State Validation ===");

    /* ---- CR / EFER ---- */
    u64 g_cr0 = vmx_vmcs_read(VMCS_GUEST_CR0);
    u64 g_cr3 = vmx_vmcs_read(VMCS_GUEST_CR3);
    u64 g_cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    u64 g_efer = vmx_vmcs_read(VMCS_GUEST_IA32_EFER);
    log_hex64("[VMM] gCR0=", g_cr0);
    log_hex64("[VMM] gCR3=", g_cr3);
    log_hex64("[VMM] gCR4=", g_cr4);
    log_hex64("[VMM] gEFER=", g_efer);

    /* ---- RIP/RSP/RFLAGS ---- */
    log_hex64("[VMM] gRIP=", vmx_vmcs_read(VMCS_GUEST_RIP));
    log_hex64("[VMM] gRSP=", vmx_vmcs_read(VMCS_GUEST_RSP));
    log_hex64("[VMM] gRFLAGS=", vmx_vmcs_read(VMCS_GUEST_RFLAGS));

    /* ---- Segment selectors ---- */
    u64 es = vmx_vmcs_read(VMCS_GUEST_ES_SELECTOR);
    u64 cs = vmx_vmcs_read(VMCS_GUEST_CS_SELECTOR);
    u64 ss = vmx_vmcs_read(VMCS_GUEST_SS_SELECTOR);
    u64 ds = vmx_vmcs_read(VMCS_GUEST_DS_SELECTOR);
    u64 fs = vmx_vmcs_read(VMCS_GUEST_FS_SELECTOR);
    u64 gs = vmx_vmcs_read(VMCS_GUEST_GS_SELECTOR);
    u64 ldtr = vmx_vmcs_read(VMCS_GUEST_LDTR_SELECTOR);
    u64 tr = vmx_vmcs_read(VMCS_GUEST_TR_SELECTOR);
    log_hex64("[VMM] sSEL ES=", es); log_hex64("[VMM] sSEL CS=", cs);
    log_hex64("[VMM] sSEL SS=", ss); log_hex64("[VMM] sSEL DS=", ds);
    log_hex64("[VMM] sSEL FS=", fs); log_hex64("[VMM] sSEL GS=", gs);
    log_hex64("[VMM] sSEL LDTR=", ldtr); log_hex64("[VMM] sSEL TR=", tr);

    /* ---- Segment limits ---- */
    log_hex64("[VMM] sLIM ES=", vmx_vmcs_read(VMCS_GUEST_ES_LIMIT));
    log_hex64("[VMM] sLIM CS=", vmx_vmcs_read(VMCS_GUEST_CS_LIMIT));
    log_hex64("[VMM] sLIM SS=", vmx_vmcs_read(VMCS_GUEST_SS_LIMIT));
    log_hex64("[VMM] sLIM DS=", vmx_vmcs_read(VMCS_GUEST_DS_LIMIT));
    log_hex64("[VMM] sLIM FS=", vmx_vmcs_read(VMCS_GUEST_FS_LIMIT));
    log_hex64("[VMM] sLIM GS=", vmx_vmcs_read(VMCS_GUEST_GS_LIMIT));
    log_hex64("[VMM] sLIM LDTR=", vmx_vmcs_read(VMCS_GUEST_LDTR_LIMIT));
    log_hex64("[VMM] sLIM TR=", vmx_vmcs_read(VMCS_GUEST_TR_LIMIT));

    /* ---- Segment access rights ---- */
    log_hex64("[VMM] sACC ES=", vmx_vmcs_read(VMCS_GUEST_ES_ACCESS));
    log_hex64("[VMM] sACC CS=", vmx_vmcs_read(VMCS_GUEST_CS_ACCESS));
    log_hex64("[VMM] sACC SS=", vmx_vmcs_read(VMCS_GUEST_SS_ACCESS));
    log_hex64("[VMM] sACC DS=", vmx_vmcs_read(VMCS_GUEST_DS_ACCESS));
    log_hex64("[VMM] sACC FS=", vmx_vmcs_read(VMCS_GUEST_FS_ACCESS));
    log_hex64("[VMM] sACC GS=", vmx_vmcs_read(VMCS_GUEST_GS_ACCESS));
    log_hex64("[VMM] sACC LDTR=", vmx_vmcs_read(VMCS_GUEST_LDTR_ACCESS));
    log_hex64("[VMM] sACC TR=", vmx_vmcs_read(VMCS_GUEST_TR_ACCESS));

    /* ---- Segment bases ---- */
    log_hex64("[VMM] sBAS CS=", vmx_vmcs_read(VMCS_GUEST_CS_BASE));
    log_hex64("[VMM] sBAS TR=", vmx_vmcs_read(VMCS_GUEST_TR_BASE));
    log_hex64("[VMM] sBAS GDTR=", vmx_vmcs_read(VMCS_GUEST_GDTR_BASE));
    log_hex64("[VMM] sBAS IDTR=", vmx_vmcs_read(VMCS_GUEST_IDTR_BASE));

    /* ---- GDTR/IDTR limits ---- */
    log_hex64("[VMM] dLIM GDTR=", vmx_vmcs_read(VMCS_GUEST_GDTR_LIMIT));
    log_hex64("[VMM] dLIM IDTR=", vmx_vmcs_read(VMCS_GUEST_IDTR_LIMIT));

    /* ---- Activity / interruptibility / link ---- */
    log_hex64("[VMM] gACT=", vmx_vmcs_read(VMCS_GUEST_ACTIVITY_STATE));
    log_hex64("[VMM] gINTSTATE=", vmx_vmcs_read(VMCS_GUEST_INTERRUPTIBILITY));
    log_hex64("[VMM] gLINK=", vmx_vmcs_read(VMCS_GUEST_LINK_POINTER));

    /* ---- SYSENTER ---- */
    log_hex64("[VMM] gSYSENTER_CS=", vmx_vmcs_read(VMCS_GUEST_SYSENTER_CS));
    log_hex64("[VMM] gDR7=", vmx_vmcs_read(VMCS_GUEST_DR7));

    /* ---- Host state summary ---- */
    log_hex64("[VMM] hCR0=", vmx_vmcs_read(VMCS_HOST_CR0));
    log_hex64("[VMM] hCR3=", vmx_vmcs_read(VMCS_HOST_CR3));
    log_hex64("[VMM] hCR4=", vmx_vmcs_read(VMCS_HOST_CR4));
    log_hex64("[VMM] hRIP=", vmx_vmcs_read(VMCS_HOST_RIP));
    log_hex64("[VMM] hRSP=", vmx_vmcs_read(VMCS_HOST_RSP));
    log_hex64("[VMM] hEFER=", vmx_vmcs_read(VMCS_HOST_IA32_EFER));

    log_info("[VMM] === End Guest State ===");
}

int vmm_self_test(void) {
    if (!g_vmm_ready) {
        log_error("[VMM] not ready");
        return -1;
    }

    log_info("[VMM] self-test begin");

    /* 重置状态 */
    g_guest_terminated = 0;
    g_last_exit_reason = 0xFFFFFFFFULL;

    /* 构建 guest GDT + TSS（满足 VMX 对 guest TR 非空的要求） */
    build_guest_gdt();

    /* EPT 映射 guest 代码页（GPA 0x1000）、栈页（GPA 0x8000）、
     * GDT 页（GPA 0x3000）、TSS 页（GPA 0x4000）。
     * g_guest_* 属于高半 UTSM 内核映像，必须使用 Limine 提供的
     * kernel virtual/physical base 转换；它们不是 HHDM 指针。 */
    u64 guest_code_phys = vmx_kernel_virt_to_phys(g_guest_code);
    u64 guest_stack_phys = vmx_kernel_virt_to_phys(g_guest_stack);
    u64 guest_gdt_phys = vmx_kernel_virt_to_phys(g_guest_gdt);
    u64 guest_tss_phys = vmx_kernel_virt_to_phys(g_guest_tss);

    if (ept_map_range(0x1000, guest_code_phys, 4096, EPT_RWX) != 0) {
        log_error("[VMM] map guest code failed");
        return -3;
    }
    if (ept_map_range(0x8000, guest_stack_phys, 4096, EPT_RWX) != 0) {
        log_error("[VMM] map guest stack failed");
        return -4;
    }
    if (ept_map_range(0x3000, guest_gdt_phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[VMM] map guest GDT failed");
        return -5;
    }
    if (ept_map_range(0x4000, guest_tss_phys, 4096, EPT_READ | EPT_WRITE) != 0) {
        log_error("[VMM] map guest TSS failed");
        return -6;
    }
    /* P8.4: Map IDT page for KVM nested VMX (IDTR base = 0x5000) */
    {
        u64 guest_idt_phys = vmx_kernel_virt_to_phys(g_guest_idt);
        for (int i = 0; i < 4096; i++) g_guest_idt[i] = 0;
        if (ept_map_range(0x5000, guest_idt_phys, 4096, EPT_READ | EPT_WRITE) != 0) {
            log_error("[VMM] map guest IDT failed");
            return -9;
        }
    }

    /* P8.5: 构建 guest 自有页表（GPA 低 2MB 恒等，2MB 大页）并 EPT 映射。
     * 页表项：P(bit0)|RW(bit1)，大页加 PS(bit7)；NX=0（可执行）。 */
    {
        for (int i = 0; i < 512; i++) {
            g_guest_pml4[i] = 0;
            g_guest_pdpt[i] = 0;
            g_guest_pd[i] = 0;
        }
        g_guest_pml4[0] = 0x7000 | 0x3;   /* P|RW -> PDPT @ GPA 0x7000 */
        g_guest_pdpt[0] = 0x9000 | 0x3;   /* P|RW -> PD   @ GPA 0x9000 */
        g_guest_pd[0]   = 0x0000 | 0x83;  /* P|RW|PS -> 2MB page @ GPA 0 */

        u64 pml4_phys = vmx_kernel_virt_to_phys(g_guest_pml4);
        u64 pdpt_phys = vmx_kernel_virt_to_phys(g_guest_pdpt);
        u64 pd_phys   = vmx_kernel_virt_to_phys(g_guest_pd);
        if (ept_map_range(0x6000, pml4_phys, 4096, EPT_READ | EPT_WRITE) != 0 ||
            ept_map_range(0x7000, pdpt_phys, 4096, EPT_READ | EPT_WRITE) != 0 ||
            ept_map_range(0x9000, pd_phys, 4096, EPT_READ | EPT_WRITE) != 0) {
            log_error("[VMM] map guest page tables failed");
            return -12;
        }
    }

    /* 配置 VMCS */
    vmcs_setup_guest_state_phase1();
    vmcs_setup_controls(ept_get_eptp());
    vmcs_setup_host_state();

    /* P8.4: VMCLEAR + VMPTRLD to put VMCS in "clear" state before VMLAUNCH.
     * Without this, VMLAUNCH fails (error 4 or silent failure under KVM). */
    if (vmx_vmcs_clear(g_vmcs_phys) != 0) {
        log_error("[VMM] vmclear failed");
        return -10;
    }
    if (vmx_vmcs_load(g_vmcs_phys) != 0) {
        log_error("[VMM] vmptrld after vmclear failed");
        return -11;
    }

    log_info("[VMM] launching guest");

    /* P8.4: dump key VMCS fields before VMLAUNCH for debugging */
    log_hex64("[VMM] pin=", vmx_vmcs_read(VMCS_PIN_BASED_VM_EXEC_CONTROL));
    log_hex64("[VMM] cpu=", vmx_vmcs_read(VMCS_CPU_BASED_VM_EXEC_CONTROL));
    log_hex64("[VMM] cpu2=", vmx_vmcs_read(VMCS_SECONDARY_VM_EXEC_CONTROL));
    log_hex64("[VMM] exit=", vmx_vmcs_read(VMCS_VM_EXIT_CONTROLS));
    log_hex64("[VMM] entry=", vmx_vmcs_read(VMCS_VM_ENTRY_CONTROLS));
    log_hex64("[VMM] eptp=", vmx_vmcs_read(VMCS_EPT_POINTER));
    log_hex64("[VMM] msr_bitmap=", vmx_vmcs_read(VMCS_MSR_BITMAP));
    log_hex64("[VMM] vpid=", vmx_vmcs_read(VMCS_VPID));
    log_hex64("[VMM] link=", vmx_vmcs_read(VMCS_GUEST_LINK_POINTER));

    /* P8.4: Dump all guest-state fields for KVM debug */
    vmm_dump_guest_state();

    /* vmlaunch 控制流：
     *   - 失败：执行下一条指令，failed=1（同时捕获 RFLAGS 区分
     *     VMfailValid CF=1 / VMfailInvalid ZF=1）
     *   - 成功：进入 guest，不返回。VM-Exit 后跳到 vmx_vm_exit_handler。
     *   - guest 终止时，handler 恢复 saved_rsp 并 jmp 到 post_guest 标签。
     *
     * 我们在 vmlaunch 前保存 RSP 与"vmlaunch 后"的 RIP（= post_guest 标签地址）。 */
    int failed;
    u64 vm_flags = 0;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"      /* 保存当前 RSP */
        "leaq 1f(%%rip), %%rax\n\t"                     /* 取 post_guest 标签地址 */
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"     /* 保存返回 RIP */
        "vmlaunch\n\t"                                  /* 启动 guest */
        /* ---- 失败路径（mov 不改 flags，先存 failed 再抓 RFLAGS） ---- */
        "movl $1, %0\n\t"
        "pushfq\n\t"
        "popq %1\n\t"
        "jmp 2f\n\t"
        /* ---- post_guest 标签：终止路径 jmp 到这里 ---- */
        "1:\n\t"
        "movl $0, %0\n\t"
        "2:\n\t"
        : "=r"(failed), "=r"(vm_flags)
        :: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
    );

    if (failed) {
        /* CF(bit0)=1 → VMfailInvalid（current-VMCS 无效）；
         * ZF(bit6)=1 → VMfailValid（错误码在 VMCS 0x4400）。 */
        if (vm_flags & 0x1) {
            log_error("[VMM] vmlaunch VMfailInvalid (invalid current-VMCS)");
        } else if (vm_flags & 0x40) {
            log_error("[VMM] vmlaunch VMfailValid");
        } else {
            log_error("[VMM] vmlaunch failed (unexpected flags)");
        }
        u64 error = 0;
        if (!(vm_flags & 0x40)) {
            log_error("[VMM] VM-instruction error unavailable");
        } else if (vmx_vmcs_read_checked(VMCS_VMX_INSTRUCTION_ERROR, &error) != 0) {
            log_error("[VMM] VMREAD error field failed (current-VMCS invalid)");
        }
        log_hex64("[VMM] vmlaunch failed, error=", error);
        serial_write("[VMM] ");
        serial_write(vmx_error_str(error));
        serial_putc('\n');

        /* P8.4: diagnostic - dump guest CR0/CR4 vs VMX fixed bits */
        {
            u64 cr0_fixed0 = vmx_read_msr(0x486);  /* IA32_VMX_CR0_FIXED0 */
            u64 cr0_fixed1 = vmx_read_msr(0x487);  /* IA32_VMX_CR0_FIXED1 */
            u64 cr4_fixed0 = vmx_read_msr(0x488);  /* IA32_VMX_CR4_FIXED0 */
            u64 cr4_fixed1 = vmx_read_msr(0x489);  /* IA32_VMX_CR4_FIXED1 */
            u64 g_cr0 = vmx_vmcs_read(VMCS_GUEST_CR0);
            u64 g_cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
            log_hex64("[VMM] CR0_FIXED0=", cr0_fixed0);
            log_hex64("[VMM] CR0_FIXED1=", cr0_fixed1);
            log_hex64("[VMM] guest CR0  =", g_cr0);
            log_hex64("[VMM] CR4_FIXED0=", cr4_fixed0);
            log_hex64("[VMM] CR4_FIXED1=", cr4_fixed1);
            log_hex64("[VMM] guest CR4  =", g_cr4);
            /* Check: guest CR0 must have all bits in CR0_FIXED0 set,
             * and must not have any bits set that are 0 in CR0_FIXED1 */
            u64 cr0_missing = cr0_fixed0 & ~g_cr0;  /* bits FIXED0 requires but guest lacks */
            u64 cr0_extra = g_cr0 & ~cr0_fixed1;     /* bits guest has but FIXED1 forbids */
            log_hex64("[VMM] CR0 missing(need)=", cr0_missing);
            log_hex64("[VMM] CR0 extra(forbid)=", cr0_extra);
            u64 cr4_missing = cr4_fixed0 & ~g_cr4;
            u64 cr4_extra = g_cr4 & ~cr4_fixed1;
            log_hex64("[VMM] CR4 missing(need)=", cr4_missing);
            log_hex64("[VMM] CR4 extra(forbid)=", cr4_extra);
        }
        return -7;
    }

    /* 到达这里：guest 已终止（HLT 被处理） */
    log_info("[VMM] guest terminated successfully");
    log_hex64("[VMM] last exit reason=", g_last_exit_reason);
    log_hex64("[VMM] vmexit count=", vmexit_get_count());

    if (g_last_exit_reason != EXIT_HLT) {
        log_warn("[VMM] expected HLT exit");
        return -8;
    }

    log_info("[VMM] self-test PASS");
    return 0;
}

/* 由 vmexit_asm.S 调用：dispatch 返回 1 = vmresume，返回 0 = 终止。
 * 终止时，asm 侧恢复 saved_rsp 并 jmp saved_return_rip。
 *
 * 注意：vmresume 现在在 vmexit_asm.S 中执行（在恢复 guest GPR 之后），
 * 不再在 C 中调用。这样 C 处理器可以修改 g_guest_regs.rax 等，
 * 汇编恢复后 vmresume 时 CPU GPR = 修改后的 guest 值。 */
void vmm_vmexit_entry(void) {
    int resume = vmexit_dispatch();
    g_last_exit_reason = vmx_vmcs_read(VMCS_EXIT_REASON) & 0xFFFF;

    if (resume) {
        /* 汇编侧会恢复 GPR 并执行 vmresume，无需在此调用。
         * 如果 vmresume 失败，汇编会设置 g_guest_terminated=1。 */
    } else {
        g_guest_terminated = 1;
    }
}

/* 供 vmexit_asm.S 读取的全局变量符号已在文件顶部声明为非 static 全局变量。 */
