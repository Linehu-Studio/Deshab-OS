#ifndef UTSM_VMX_H
#define UTSM_VMX_H

#include <utsm/types.h>

/* ===== VMX MSRs ===== */
#define IA32_VMX_BASIC               0x480
#define IA32_VMX_PINBASED_CTLS       0x481
#define IA32_VMX_PROCBASED_CTLS      0x482
#define IA32_VMX_EXIT_CTLS           0x483
#define IA32_VMX_ENTRY_CTLS          0x484
#define IA32_VMX_PROCBASED_CTLS2     0x48B
#define IA32_VMX_EPT_VPID_CAP        0x48C
#define IA32_VMX_TRUE_PINBASED_CTLS  0x48D
#define IA32_VMX_TRUE_PROCBASED_CTLS 0x48E
#define IA32_VMX_TRUE_EXIT_CTLS      0x48F
#define IA32_VMX_TRUE_ENTRY_CTLS     0x490

/* ===== CR4 / CR0 bits ===== */
#define CR4_VMXE_BIT                 13ULL
#define CR4_VMXE                     (1ULL << CR4_VMXE_BIT)
#define CR4_PAE                      (1ULL << 5)
#define CR4_PGE                      (1ULL << 7)
#define CR4_PSE                      (1ULL << 4)

#define CR0_PE                       (1ULL << 0)
#define CR0_NE                       (1ULL << 5)
#define CR0_WP                       (1ULL << 16)
#define CR0_PG                       (1ULL << 31)

/* ===== VMCS field encodings (Intel SDM Vol 3, Appendix H) ===== */
/* Field encoding: bit 13=1 -> host root, bit 12=1 -> guest, bits [11:10] = type, bits [9:1] = index */
#define VMCS_FIELD_HOST          0x2000ULL
#define VMCS_FIELD_GUEST         0x1000ULL
#define VMCS_FIELD_CTRL          0x4000ULL
#define VMCS_FIELD_READ          0x6000ULL

/* Control fields */
#define VMCS_VPID                        0x0000
#define VMCS_GUEST_ES_ADDR               0x0006
#define VMCS_GUEST_CS_ADDR               0x0008
#define VMCS_GUEST_SS_ADDR               0x000A
#define VMCS_GUEST_DS_ADDR               0x000C
#define VMCS_GUEST_FS_ADDR               0x000E
#define VMCS_GUEST_GS_ADDR               0x0010
#define VMCS_GUEST_LDTR_ADDR             0x0012
#define VMCS_GUEST_TR_ADDR               0x0014
#define VMCS_GUEST_INTR_STATUS           0x0009
#define VMCS_IO_BITMAP_A                 0x2000
#define VMCS_IO_BITMAP_B                 0x2002
#define VMCS_MSR_BITMAP                  0x2004
#define VMCS_TSC_OFFSET                  0x2010
#define VMCS_EPT_POINTER                 0x201A

/* Read-only data fields */
#define VMCS_GUEST_PHYSICAL_ADDR         0x2400
#define VMCS_EXIT_REASON                 0x4402
#define VMCS_EXIT_QUALIFICATION          0x6400
#define VMCS_IO_RCX                      0x6404
#define VMCS_IO_RSI                      0x6406
#define VMCS_IO_RDI                      0x6408
#define VMCS_IO_RIP                      0x640A
#define VMCS_GUEST_LINEAR_ADDR           0x640A
#define VMCS_INSTRUCTION_LENGTH          0x440C
#define VMCS_VMX_INSTRUCTION_ERROR       0x4406

/* Guest-state fields */
#define VMCS_GUEST_ES_SELECTOR           0x0800
#define VMCS_GUEST_CS_SELECTOR           0x0802
#define VMCS_GUEST_SS_SELECTOR           0x0804
#define VMCS_GUEST_DS_SELECTOR           0x0806
#define VMCS_GUEST_FS_SELECTOR           0x0808
#define VMCS_GUEST_GS_SELECTOR           0x080A
#define VMCS_GUEST_LDTR_SELECTOR         0x080C
#define VMCS_GUEST_TR_SELECTOR           0x080E
#define VMCS_GUEST_ES_LIMIT              0x4800
#define VMCS_GUEST_CS_LIMIT              0x4802
#define VMCS_GUEST_SS_LIMIT              0x4804
#define VMCS_GUEST_DS_LIMIT              0x4806
#define VMCS_GUEST_FS_LIMIT              0x4808
#define VMCS_GUEST_GS_LIMIT              0x480A
#define VMCS_GUEST_LDTR_LIMIT            0x480C
#define VMCS_GUEST_TR_LIMIT              0x480E
#define VMCS_GUEST_CS_ACCESS             0x4816
#define VMCS_GUEST_SS_ACCESS             0x481A
#define VMCS_GUEST_DS_ACCESS             0x4818
#define VMCS_GUEST_ES_ACCESS             0x4814
#define VMCS_GUEST_FS_ACCESS             0x481C
#define VMCS_GUEST_GS_ACCESS             0x481E
#define VMCS_GUEST_LDTR_ACCESS           0x4820
#define VMCS_GUEST_TR_ACCESS             0x4822
#define VMCS_GUEST_INTERRUPTIBILITY      0x4824
#define VMCS_GUEST_ACTIVITY_STATE        0x4826
#define VMCS_GUEST_SYSENTER_CS           0x482A
#define VMCS_GUEST_PREEMPTION_TIMER      0x482E

/* 64-bit guest-state fields（与 PDPTR 共享编码，仅在 EPT 启用时为 EFER/PAT） */
#define VMCS_GUEST_LINK_POINTER         0x2800  /* P8.4: must be 0xFFFFFFFFFFFFFFFF */
#define VMCS_GUEST_IA32_EFER             0x2806
#define VMCS_GUEST_IA32_PAT              0x2804
#define VMCS_GUEST_IA32_PERF_GLOBAL_CTRL 0x2808
#define VMCS_GUEST_CR0                   0x6800
#define VMCS_GUEST_CR3                   0x6802
#define VMCS_GUEST_CR4                   0x6804
#define VMCS_GUEST_ES_BASE               0x6806
#define VMCS_GUEST_CS_BASE               0x6808
#define VMCS_GUEST_SS_BASE               0x680A
#define VMCS_GUEST_DS_BASE               0x680C
#define VMCS_GUEST_FS_BASE               0x680E
#define VMCS_GUEST_GS_BASE               0x6810
#define VMCS_GUEST_LDTR_BASE             0x6812
#define VMCS_GUEST_TR_BASE               0x6814
#define VMCS_GUEST_GDTR_BASE             0x6816
#define VMCS_GUEST_GDTR_LIMIT            0x4810
#define VMCS_GUEST_IDTR_BASE             0x6818
#define VMCS_GUEST_IDTR_LIMIT            0x4812
#define VMCS_GUEST_DR7                   0x681A
#define VMCS_GUEST_RSP                   0x681C
#define VMCS_GUEST_RIP                   0x681E
#define VMCS_GUEST_RFLAGS                0x6820
#define VMCS_GUEST_SYSENTER_ESP          0x6824
#define VMCS_GUEST_SYSENTER_EIP          0x6826

/* Host-state fields (Intel SDM Vol 3D, Appendix H, Table H-6) */
#define VMCS_HOST_CR0                    0x6C00
#define VMCS_HOST_CR3                    0x6C02
#define VMCS_HOST_CR4                    0x6C04
#define VMCS_HOST_ES_SELECTOR            0x0C00
#define VMCS_HOST_CS_SELECTOR            0x0C02
#define VMCS_HOST_SS_SELECTOR            0x0C04
#define VMCS_HOST_DS_SELECTOR            0x0C06
#define VMCS_HOST_FS_SELECTOR            0x0C08
#define VMCS_HOST_GS_SELECTOR            0x0C0A
#define VMCS_HOST_TR_SELECTOR            0x0C0C
#define VMCS_HOST_FS_BASE                0x6C06
#define VMCS_HOST_GS_BASE                0x6C08
#define VMCS_HOST_TR_BASE                0x6C0A
#define VMCS_HOST_GDTR_BASE              0x6C0C
#define VMCS_HOST_IDTR_BASE              0x6C0E
#define VMCS_HOST_SYSENTER_CS            0x4C00
#define VMCS_HOST_RSP                    0x6C14
#define VMCS_HOST_RIP                    0x6C16
#define VMCS_HOST_SYSENTER_ESP           0x6C10
#define VMCS_HOST_SYSENTER_EIP           0x6C12

/* 64-bit host-state fields */
#define VMCS_HOST_IA32_EFER              0x2C02
#define VMCS_HOST_IA32_PAT               0x2C00
#define VMCS_HOST_IA32_PERF_GLOBAL_CTRL  0x2C04

/* IA32_EFER MSR (0xC0000080) bits */
#define EFER_SCE                         (1ULL << 0)
#define EFER_LME                         (1ULL << 8)
#define EFER_LMA                         (1ULL << 10)
#define EFER_NXE                         (1ULL << 11)
#define EFER_SVME                        (1ULL << 12)
#define EFER_LMSLE                       (1ULL << 13)
#define EFER_FFXSR                       (1ULL << 14)
#define EFER_TCE                         (1ULL << 15)

/* VM-execution control fields */
#define VMCS_PIN_BASED_VM_EXEC_CONTROL   0x4000
#define VMCS_CPU_BASED_VM_EXEC_CONTROL   0x4002
#define VMCS_EXCEPTION_BITMAP            0x4004
#define VMCS_VM_EXIT_CONTROLS            0x400C
#define VMCS_VM_ENTRY_CONTROLS           0x4012
#define VMCS_VM_ENTRY_INTERRUPT_INFO     0x4016
#define VMCS_SECONDARY_VM_EXEC_CONTROL   0x401E

/* ===== Pin-based controls ===== */
#define PIN_EXT_INTERRUPT_EXITING        (1ULL << 0)
#define PIN_NMI_EXITING                  (1ULL << 3)
#define PIN_VMX_PREEMPTION_TIMER         (1ULL << 6)

/* ===== Primary processor-based controls ===== */
#define CPU_BASED_INTR_WINDOW_EXITING    (1ULL << 2)
#define CPU_BASED_HLT_EXITING            (1ULL << 7)
#define CPU_BASED_INVLPG_EXITING         (1ULL << 9)
#define CPU_BASED_MWAIT_EXITING          (1ULL << 10)
#define CPU_BASED_RDPMC_EXITING          (1ULL << 11)
#define CPU_BASED_RDTSC_EXITING          (1ULL << 12)
#define CPU_BASED_CR8_LOAD_EXITING       (1ULL << 19)
#define CPU_BASED_CR8_STORE_EXITING      (1ULL << 20)
#define CPU_BASED_MOV_DR_EXITING         (1ULL << 23)
#define CPU_BASED_UNCOND_IO_EXITING      (1ULL << 24)
#define CPU_BASED_USE_IO_BITMAPS         (1ULL << 25)
#define CPU_BASED_PAUSE_EXITING          (1ULL << 30)
#define CPU_BASED_ACTIVATE_SECONDARY     (1ULL << 31)
#define CPU_BASED_USE_MSR_BITMAPS        (1ULL << 28)

/* ===== Secondary processor-based controls ===== */
#define SEC_EXEC_ENABLE_EPT              (1ULL << 1)
#define SEC_EXEC_ENABLE_VPID             (1ULL << 5)
#define SEC_EXEC_UNRESTRICTED_GUEST      (1ULL << 7)

/* ===== VM-exit controls ===== */
#define VM_EXIT_SAVE_DEBUG_CONTROLS      (1ULL << 2)
#define VM_EXIT_HOST_ADDR_SPACE_SIZE     (1ULL << 9)
#define VM_EXIT_LOAD_PERF_GLOBAL_CTRL    (1ULL << 12)
#define VM_EXIT_ACK_INTR_ON_EXIT         (1ULL << 15)
#define VM_EXIT_SAVE_GUEST_EFER          (1ULL << 20)
#define VM_EXIT_LOAD_HOST_EFER           (1ULL << 21)

/* ===== VM-entry controls ===== */
#define VM_ENTRY_LOAD_DEBUG_CONTROLS     (1ULL << 2)
#define VM_ENTRY_IA32E_MODE_GUEST        (1ULL << 9)
#define VM_ENTRY_LOAD_GUEST_EFER         (1ULL << 15)

/* ===== Exit reasons ===== */
#define EXIT_EXCEPTION_NMI               0
#define EXIT_EXTERNAL_INTERRUPT          1
#define EXIT_TRIPLE_FAULT                2
#define EXIT_INIT_SIGNAL                 3
#define EXIT_INTERRUPT_WINDOW            7
#define EXIT_CPUID                       10
#define EXIT_INVD                        13
#define EXIT_RDMSR                       31
#define EXIT_WRMSR                       32
#define EXIT_MONITOR                     39
#define EXIT_MWAIT                       36
#define EXIT_VMCALL                      18
#define EXIT_VMCLEAR                     19
#define EXIT_VMLAUNCH                    20
#define EXIT_VMPTRLD                     21
#define EXIT_VMPTRST                     22
#define EXIT_VMREAD                      23
#define EXIT_VMRESUME                    24
#define EXIT_VMWRITE                     25
#define EXIT_VMXOFF                      26
#define EXIT_VMXON                       27
#define EXIT_CONTROL_ACCESS              28
#define EXIT_IO_INSTRUCTION              30
#define EXIT_RDPMC                       15
#define EXIT_RDTSC                       16
#define EXIT_HLT                         12
#define EXIT_INVLPG                      14
#define EXIT_EPT_VIOLATION               48
#define EXIT_EPT_MISCONFIG               49
#define EXIT_VMX_PREEMPTION_TIMER        52
#define EXIT_APIC_ACCESS                 44

/* ===== VM-entry interruption-information field 编码 ===== */
#define VM_ENTRY_INTR_INFO_VALID         0x80000000ULL
#define VM_ENTRY_INTR_TYPE_HW_IRQ        0   /* external interrupt (bits 10:8) */

/* ===== EPT memory types ===== */
#define EPT_MEMORY_TYPE_UC               0ULL
#define EPT_MEMORY_TYPE_WC               1ULL
#define EPT_MEMORY_TYPE_WT               4ULL
#define EPT_MEMORY_TYPE_WP               5ULL
#define EPT_MEMORY_TYPE_WB               6ULL

/* ===== EPT entry bits ===== */
#define EPT_READ                         (1ULL << 0)
#define EPT_WRITE                        (1ULL << 1)
#define EPT_EXECUTE                      (1ULL << 2)
#define EPT_RWX                          (EPT_READ | EPT_WRITE | EPT_EXECUTE)
#define EPT_LARGE_PAGE                   (1ULL << 7)

/* ===== VMX instruction errors ===== */
#define VMXERR_VMCALL_INVALID            1

/* ===== Public API ===== */
int vmx_supported(void);
int vmx_enable(void);
int vmx_disable(void);
int vmx_vmcs_alloc(u64 *phys_out);
int vmx_vmcs_load(u64 phys);
int vmx_vmcs_clear(u64 phys);  /* P8.4: VMCLEAR before VMLAUNCH */
u64 vmx_vmcs_read(u64 field);
void vmx_vmcs_write(u64 field, u64 value);
int vmx_vmlaunch(void);
int vmx_vmresume(void);

u64 vmx_read_msr(u32 msr);
u64 vmx_get_host_cr3(void);

/* ===== 虚拟中断注入（vmexit.c 实现） ===== */
/* 向 guest 队列注入一个 ISA IRQ 向量（legacy PIC：vector = 0x30 + irq）。
 * 仅排队；实际在下一次 vmresume 前（guest RFLAGS.IF=1 且无阻塞时）注入，
 * 否则 arm interrupt-window exiting，待 guest 可接收时注入。 */
void vmx_guest_queue_irq(u32 vector);

/* VMX preemption timer：返回 1ms 对应的 timer 计数值（按 IA32_VMX_BASIC
 * bits[55:48] 的 scale 换算 TSC 频率）。 */
u64 vmx_preemption_quantum_1ms(void);
/* 检查 CPU 是否支持 VMX preemption timer。 */
int vmx_preemption_timer_supported(void);

#endif
