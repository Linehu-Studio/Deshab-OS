#include <utsm/vmm.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== VMX 指令错误码 ===== */
static const char *vmx_error_str(u64 err) {
    switch (err) {
    case 1: return "VMCALL/VMCLEAR/VMLAUNCH/VMRESUME with non-launched VMCS";
    case 2: return "VMRESUME after VMXOFF";
    case 3: return "VMRESUME with corrupted VMCS";
    case 4: return "vmlaunch with non-clear VMCS";
    case 5: return "VM operation invalid";
    case 6: return "VMCS revision mismatch";
    case 7: return "VMCS shadowing mismatch";
    case 8: return "invalid guest state";
    case 9: return "host state invalid";
    case 10: return "VM-execution control invalid";
    case 11: return "VM-exit control invalid";
    case 12: return "VM-entry control invalid";
    case 13: return "VM-exit control fields invalid";
    case 15: return "address out of width";
    case 16: return "MSR bitmap addr invalid";
    case 17: return "VMREAD/VMWRITE from unsupported field";
    case 18: return "VMCS addr invalid";
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

/* MSR bitmap（4KB 全 0 = 不拦截 MSR） */
static u8 g_msr_bitmap[4096] __attribute__((aligned(4096)));

/* guest GDT（GPA 0x3000）与 TSS（GPA 0x4000）。
 * VMX 要求 guest TR 非空且引用 GDT 中的 TSS 描述符。 */
static u8 g_guest_gdt[4096] __attribute__((aligned(4096)));
static u8 g_guest_tss[4096] __attribute__((aligned(4096)));

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

static u64 vmx_adjust_control(u64 value, u32 msr) {
    u32 allowed0 = (u32)vmx_read_msr(msr);
    u32 allowed1 = (u32)(vmx_read_msr(msr) >> 32);
    u32 adjusted = (u32)value | allowed0;
    adjusted &= allowed1;
    return adjusted;
}

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
    /* Guest CR0/CR3/CR4：启用 PE/NE/PG/PAE，复用 host 页表 */
    vmx_vmcs_write(VMCS_GUEST_CR0, CR0_PE | CR0_NE | CR0_PG | CR0_WP);
    vmx_vmcs_write(VMCS_GUEST_CR3, read_cr3_local());
    vmx_vmcs_write(VMCS_GUEST_CR4, CR4_PAE | CR4_PGE | CR4_PSE);

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
    vmx_vmcs_write(VMCS_GUEST_IDTR_BASE, 0);

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
     * DS/SS/ES/FS/GS: 0xC093 = present, DPL0, data, writable
     * LDTR: 0x82 = unusable (L=0 in high nibble)
     * TR: 0x8B = present, DPL0, 64-bit busy TSS */
    vmx_vmcs_write(VMCS_GUEST_CS_ACCESS, 0xA09B);
    vmx_vmcs_write(VMCS_GUEST_SS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_DS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_ES_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_FS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_GS_ACCESS, 0xC093);
    vmx_vmcs_write(VMCS_GUEST_LDTR_ACCESS, 0x82);
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

    /* Guest activity state = 0 (active) */
    vmx_vmcs_write(VMCS_GUEST_ACTIVITY_STATE, 0);
    /* Guest interruptibility = 0（无阻塞） */
    vmx_vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

static void vmcs_setup_controls(u64 eptp) {
    u64 pin = vmx_adjust_control(PIN_EXT_INTERRUPT_EXITING | PIN_NMI_EXITING,
                                 IA32_VMX_TRUE_PINBASED_CTLS);

    u64 cpu = CPU_BASED_HLT_EXITING
            | CPU_BASED_ACTIVATE_SECONDARY
            | CPU_BASED_USE_MSR_BITMAPS
            | CPU_BASED_UNCOND_IO_EXITING
            | CPU_BASED_INVLPG_EXITING;
    cpu = vmx_adjust_control(cpu, IA32_VMX_TRUE_PROCBASED_CTLS);

    /* Secondary controls：启用 EPT + VPID */
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

    /* MSR bitmap 物理地址 */
    {
        u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
        u64 msr_bitmap_phys = (u64)g_msr_bitmap - hhdm;
        vmx_vmcs_write(VMCS_MSR_BITMAP, msr_bitmap_phys);
    }

    vmx_vmcs_write(VMCS_EPT_POINTER, eptp);
    vmx_vmcs_write(VMCS_VPID, 1);

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
    return 0;
}

void vmm_shutdown(void) {
    if (!g_vmm_ready) return;
    vmx_disable();
    g_vmm_ready = 0;
}

int vmm_is_ready(void) { return g_vmm_ready; }
u64 vmm_get_eptp(void) { return ept_get_eptp(); }

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
     * g_guest_* 是 UTSM 虚拟地址，物理地址 = virt - HHDM */
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    u64 guest_code_phys = (u64)g_guest_code - hhdm;
    u64 guest_stack_phys = (u64)g_guest_stack - hhdm;
    u64 guest_gdt_phys = (u64)g_guest_gdt - hhdm;
    u64 guest_tss_phys = (u64)g_guest_tss - hhdm;

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

    /* 配置 VMCS */
    vmcs_setup_guest_state_phase1();
    vmcs_setup_controls(ept_get_eptp());
    vmcs_setup_host_state();

    log_info("[VMM] launching guest");

    /* vmlaunch 控制流：
     *   - 失败：执行下一条指令，failed=1
     *   - 成功：进入 guest，不返回。VM-Exit 后跳到 vmx_vm_exit_handler。
     *   - guest 终止时，handler 恢复 saved_rsp 并 jmp 到 post_guest 标签。
     *
     * 我们在 vmlaunch 前保存 RSP 与"vmlaunch 后"的 RIP（= post_guest 标签地址）。 */
    int failed;
    __asm__ volatile(
        "movq %%rsp, g_saved_host_rsp(%%rip)\n\t"      /* 保存当前 RSP */
        "leaq 1f(%%rip), %%rax\n\t"                     /* 取 post_guest 标签地址 */
        "movq %%rax, g_saved_return_rip(%%rip)\n\t"     /* 保存返回 RIP */
        "vmlaunch\n\t"                                  /* 启动 guest */
        /* ---- 失败路径 ---- */
        "movl $1, %0\n\t"
        "jmp 2f\n\t"
        /* ---- post_guest 标签：终止路径 jmp 到这里 ---- */
        "1:\n\t"
        "movl $0, %0\n\t"
        "2:\n\t"
        : "=r"(failed)
        :: "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
    );

    if (failed) {
        u64 error = vmx_vmcs_read(VMCS_VMX_INSTRUCTION_ERROR);
        log_hex64("[VMM] vmlaunch failed, error=", error);
        serial_write("[VMM] ");
        serial_write(vmx_error_str(error));
        serial_putc('\n');
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
