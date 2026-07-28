#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/hypercall.h>
#include <utsm/ipc_shm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/linux_loader.h>
#include <utsm/virtio_mmio.h>
#include "../arch/x86_64/limine.h"

/* VM-Exit 处理器：从 VMCS 读取 exit reason 与 guest 状态，分发处理。
 *
 * Phase 1.1：处理 HLT（终止 guest）、EPT violation（动态映射）、异常。
 * Phase 1.3：增加 VMCALL hypercall 处理（读 guest GPR、分发、写返回值）。
 * Phase 1.4：EPT violation 区分 RAM（按需分配 HPA）与 virtio-mmio
 *            （路由到设备模拟寄存器读写，不映射真实内存）。
 * Phase 1.5：guest 平台设备模拟（legacy 模式，配合 noapic/nolapic）：
 *            - COM1 8250 UART（console=ttyS0 输出 + host 串口输入桥）
 *            - 8259 PIC（IMR 跟踪，EOI/ICW 吞掉）
 *            - 8254 PIT ch0（divisor 跟踪，按 guest 编程频率注入 IRQ0 tick）
 *            - CPUID passthrough（屏蔽 VMX/hypervisor 位）
 *            - 虚拟中断注入（pending queue + interrupt-window exiting）
 *            - VMX preemption timer 周期 exit（轮询 host 串口 RX + tick）
 */

extern volatile struct limine_hhdm_request g_hhdm_request;

extern void vmx_vmresume_wrapper(void);

/* serial.c 导出的 host 串口与 TSC 频率接口 */
extern int  serial_try_read(void);
extern u64  serial_tsc_per_ms(void);
extern void serial_putc(char c);

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

/* ===== EPT violation: guest RAM 按需分配 =====
 *
 * guest 首次访问某 GPA 页时触发 EPT violation。我们按需分配一个
 * 4KB HPA 页并建立 GPA→HPA 映射（不再是错误的 1:1 恒等映射——
 * 那会把 guest GPA 直接当 host 物理地址，可能与 UTSM 自身内存冲突）。
 *
 * 已映射区域的访问权限不足（如先 R 映射后写）也走这里：升级为 RWX。
 */

/* 按需为普通 guest RAM GPA 分配一个 HPA 页并映射。返回 0 成功。 */
static int ept_map_guest_ram_page(u64 gpa_page) {
    dkm_dma_buffer buf;
    if (dma_alloc_pages(1, EPT_PAGE_SIZE, 0, &buf) != 0) {
        log_error("[VMEXIT] OOM for guest RAM page");
        return -1;
    }
    if (ept_map_range(gpa_page, buf.phys, EPT_PAGE_SIZE, EPT_RWX) != 0) {
        log_error("[VMEXIT] EPT map guest RAM page failed");
        return -1;
    }
    return 0;
}

/* ===== 虚拟中断注入（legacy PIC 模式，vector = 0x30 + irq） =====
 *
 * Linux 以 noapic/nolapic 启动时走 XT-PIC：master PIC 向量基址 0x30。
 * 我们向 guest 注入的外部中断只能是这些 ISA IRQ 向量：
 *   IRQ0 (0x30) PIT tick、IRQ4 (0x34) COM1、IRQ5 (0x35) virtio-blk、
 *   IRQ6 (0x36) virtio-net。
 *
 * 注入条件：guest RFLAGS.IF=1 且 interruptibility=0 且 IMR 未 mask。
 * 不满足时 arm CPU_BASED_INTR_WINDOW_EXITING，guest 一旦开中断
 * 立即产生 interrupt-window exit，届时再注入。
 */

#define IRQ_PENDING_MAX 16
static u8  g_pending_vectors[IRQ_PENDING_MAX];
static int g_pending_head, g_pending_tail;

/* guest 8259 PIC 状态（master） */
static u8  g_pic_imr = 0xFF;        /* 初始全 mask */
static int g_pic_icw_state = 0;     /* 0=就绪(OCW), 1=等ICW2, 2=等ICW3, 3=等ICW4 */
static int g_pic_icw_sngl = 0;
static int g_pic_icw_need4 = 0;

void vmx_guest_queue_irq(u32 vector) {
    /* 合并连续相同向量（PIT tick 合并 / COM1 RX 去重） */
    if (g_pending_tail != g_pending_head) {
        int last = (g_pending_tail + IRQ_PENDING_MAX - 1) % IRQ_PENDING_MAX;
        if (g_pending_vectors[last] == (u8)vector) return;
    }
    int next = (g_pending_tail + 1) % IRQ_PENDING_MAX;
    if (next == g_pending_head) return;  /* 队列满：丢弃 */
    g_pending_vectors[g_pending_tail] = (u8)vector;
    g_pending_tail = next;
}

static void maybe_inject_irq(void) {
    if (g_pending_head == g_pending_tail) return;

    u8 vec = g_pending_vectors[g_pending_head];

    /* IMR 检查：master PIC vector 0x30+n → IMR bit n */
    if (vec >= 0x30 && vec < 0x38) {
        if (g_pic_imr & (1u << (vec - 0x30))) {
            /* 被 guest mask：丢弃（级别触发设备下次 notify 会重发） */
            g_pending_head = (g_pending_head + 1) % IRQ_PENDING_MAX;
            return;
        }
    }

    u64 rflags = vmx_vmcs_read(VMCS_GUEST_RFLAGS);
    u64 intr_st = vmx_vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
    u64 cpu = vmx_vmcs_read(VMCS_CPU_BASED_VM_EXEC_CONTROL);

    if ((rflags & 0x200) && intr_st == 0) {
        /* 可注入：写 VM-entry interruption-information field */
        vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO,
                       VM_ENTRY_INTR_INFO_VALID |
                       (VM_ENTRY_INTR_TYPE_HW_IRQ << 8) | vec);
        g_pending_head = (g_pending_head + 1) % IRQ_PENDING_MAX;
        if (cpu & CPU_BASED_INTR_WINDOW_EXITING) {
            vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL,
                           cpu & ~CPU_BASED_INTR_WINDOW_EXITING);
        }
    } else {
        /* 暂不可注入：arm interrupt-window exiting */
        if (!(cpu & CPU_BASED_INTR_WINDOW_EXITING)) {
            vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL,
                           cpu | CPU_BASED_INTR_WINDOW_EXITING);
        }
    }
}

/* ===== guest COM1 8250 UART 模拟 =====
 *
 * console=ttyS0 的 I/O 端口 0x3F8-0x3FF。
 * TX：THR 写 → host 串口输出（与 UTSM log 共用 QEMU stdio）。
 * RX：host 串口输入 → g_guest_rx 环形缓冲 → guest 读 RBR；
 *     有数据时注入 IRQ4（vector 0x34）。
 * 寄存器：DLL/DLM/IER/FCR/LCR/MCR/SCR 存储，LSR/IIR/MSR 按状态合成。
 */

#define GUEST_RX_SIZE 256
static u8  g_guest_rx[GUEST_RX_SIZE];
static int g_rx_head, g_rx_tail;

static u8 g_com1_dll, g_com1_dlm, g_com1_ier, g_com1_fcr;
static u8 g_com1_lcr, g_com1_mcr, g_com1_scr;

static u64 com1_io(u32 port, int is_in, u64 value) {
    u32 reg = port - 0x3F8;
    int dlab = (g_com1_lcr & 0x80) != 0;
    if (is_in) {
        switch (reg) {
        case 0: /* RBR / DLL */
            if (dlab) return g_com1_dll;
            if (g_rx_head != g_rx_tail) {
                u8 b = g_guest_rx[g_rx_head];
                g_rx_head = (g_rx_head + 1) % GUEST_RX_SIZE;
                return b;
            }
            return 0;
        case 1: return dlab ? g_com1_dlm : g_com1_ier;
        case 2: /* IIR：bit0=0 表示有中断；bits7:6=1 表示 FIFO 启用 */
            if (g_rx_head != g_rx_tail) return 0xC4;  /* RX data available */
            return 0xC1;                              /* no interrupt pending */
        case 3: return g_com1_lcr;
        case 4: return g_com1_mcr;
        case 5: /* LSR：bit0 DR，bit5 THRE，bit6 TEMT */
            return ((g_rx_head != g_rx_tail) ? 0x01 : 0x00) | 0x20 | 0x40;
        case 6: return 0xB0;  /* MSR：CTS|DSR|DCD */
        case 7: return g_com1_scr;
        }
        return 0xFF;
    }
    switch (reg) {
    case 0:
        if (dlab) g_com1_dll = (u8)value;
        else serial_putc((char)(value & 0xFF));  /* guest → host 串口 */
        break;
    case 1: if (dlab) g_com1_dlm = (u8)value; else g_com1_ier = (u8)value; break;
    case 2: g_com1_fcr = (u8)value; break;
    case 3: g_com1_lcr = (u8)value; break;
    case 4: g_com1_mcr = (u8)value; break;
    case 7: g_com1_scr = (u8)value; break;
    }
    return 0;
}

/* ===== guest 8254 PIT ch0 模拟（仅跟踪 divisor，tick 由 preemption exit 驱动） ===== */

static u16 g_pit_divisor = 0;        /* 0 = 65536（默认 18.2Hz） */
static int g_pit_load_state = 0;     /* 0=等 lo 字节, 1=等 hi 字节 */
static u8  g_pit_lo = 0;
static u64 g_pit_period_tsc = 0;     /* 每 tick 的 TSC 周期 */
static u64 g_pit_accum = 0;
static u64 g_last_tick_tsc = 0;

static u64 rdtsc_local(void) {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static void pit_recompute_period(void) {
    u64 tpm = serial_tsc_per_ms();
    if (tpm == 0) tpm = 2000000;  /* 未校准时按 ~2GHz 兜底 */
    u64 div = g_pit_divisor ? g_pit_divisor : 65536;
    /* period_tsc = div / 1193182 Hz，用 ms 刻度换算避免溢出 */
    g_pit_period_tsc = (div * tpm) / 1193;  /* ≈ div * tpm * 1000 / 1193182 */
    if (g_pit_period_tsc == 0) g_pit_period_tsc = 1;
}

static u64 pit_io(u32 port, int is_in, u64 value) {
    if (is_in) return 0;  /* 计数器读回：恒 0（Linux 用 TSC clocksource，不依赖） */
    if (port == 0x43) {
        /* mode/command：仅关心 ch0 + lo/hi 访问方式 */
        if ((value & 0xC0) == 0x00 && (value & 0x30) == 0x30) {
            g_pit_load_state = 0;
        }
        return 0;
    }
    if (port == 0x40) {
        if (g_pit_load_state == 0) {
            g_pit_lo = (u8)value;
            g_pit_load_state = 1;
        } else {
            g_pit_divisor = (u16)(g_pit_lo | ((u16)(u8)value << 8));
            g_pit_load_state = 0;
            pit_recompute_period();
        }
        return 0;
    }
    return 0;  /* 0x41/0x42 吞掉 */
}

/* ===== 其余 legacy 端口（吞掉/返回安全值） ===== */

static u64 misc_io(u32 port, int is_in, u64 value, int *out_terminate) {
    /* 8259 PIC master */
    if (port == 0x20) {
        if (!is_in) {
            if (value & 0x10) {  /* ICW1 */
                g_pic_icw_sngl = (value & 0x02) != 0;
                g_pic_icw_need4 = (value & 0x01) != 0;
                g_pic_icw_state = 1;
                g_pic_imr = 0xFF;
            }
            /* 其余 OCW2/OCW3（含 EOI）吞掉 */
        }
        return 0;
    }
    if (port == 0x21) {
        if (is_in) return g_pic_imr;
        switch (g_pic_icw_state) {
        case 1:  /* ICW2（向量基址，忽略，固定 0x30） */
            g_pic_icw_state = g_pic_icw_sngl ? (g_pic_icw_need4 ? 3 : 0) : 2;
            break;
        case 2:  /* ICW3 */
            g_pic_icw_state = g_pic_icw_need4 ? 3 : 0;
            break;
        case 3:  /* ICW4 */
            g_pic_icw_state = 0;
            break;
        default: /* OCW1 = IMR */
            g_pic_imr = (u8)value;
            break;
        }
        return 0;
    }
    /* 8259 PIC slave：吞掉（不用） */
    if (port == 0xA0 || port == 0xA1) return is_in ? 0xFF : 0;

    /* 8254 PIT */
    if (port >= 0x40 && port <= 0x43) return pit_io(port, is_in, value);

    /* PS/2 键盘/鼠标（i8042）：status 读 0（空），data 读 0，写吞掉。
     * i8042 自检将失败 → guest 无 PS/2 键鼠（console 输入走串口）。 */
    if (port == 0x60 || port == 0x64) return is_in ? 0 : 0;

    /* CMOS/RTC */
    if (port == 0x70 || port == 0x71) return is_in ? 0 : 0;

    /* PCI config mechanism #1：对 guest 隐藏整个 PCI 总线。
     * guest reconfig BAR 会破坏 UTSM 的 AHCI/e1000，故全部返回"无设备"。 */
    if (port >= 0xCF8 && port <= 0xCFF) return is_in ? 0xFFFFFFFFULL : 0;

    /* 0xCF9 reset control：guest 请求重启 → 终止 guest 回 host */
    if (port == 0xCF9) {
        if (!is_in) {
            log_warn("[VMEXIT] guest requested reboot via 0xCF9");
            *out_terminate = 1;
        }
        return 0;
    }

    /* COM2-4：无设备 */
    if ((port >= 0x2F8 && port <= 0x2FF) ||
        (port >= 0x3E8 && port <= 0x3EF) ||
        (port >= 0x2E8 && port <= 0x2EF)) {
        return is_in ? 0xFF : 0;
    }

    /* 其余端口（0x80 delay、0x61 NMI、0x92 A20、0xB2 APM、0xF0-0xFF FPU
     * error、0xED delay、ACPI 等）：读 0，写吞掉 */
    return is_in ? 0 : 0;
}

/* ===== EXIT_IO_INSTRUCTION 处理 =====
 *
 * exit qualification（Intel SDM 27.2.4.2? 实为 Vol 3D 表 27-6）：
 *   bits 2:0  = 访问大小-1（0=1B, 1=2B, 3=4B）
 *   bit  3    = 方向（0=OUT, 1=IN）
 *   bit  4    = string 指令
 *   bit  5    = REP 前缀
 *   bits31:16 = 端口号
 * OUT 数据在 guest RAX；IN 结果写回 guest RAX。
 */
static int handle_io(u64 qualification, u64 rip, u64 instr_len, int *out_resume) {
    u32 port = (u32)(qualification >> 16);
    u32 size = (u32)(qualification & 7) + 1;
    int is_in = (qualification >> 3) & 1;
    u64 value = 0;
    int terminate = 0;

    if (!is_in) {
        value = g_guest_regs.rax;
        if (size == 1) value &= 0xFF;
        else if (size == 2) value &= 0xFFFF;
        else value &= 0xFFFFFFFFULL;
    }

    u64 result;
    if (port >= 0x3F8 && port <= 0x3FF) {
        result = com1_io(port, is_in, value);
    } else {
        result = misc_io(port, is_in, value, &terminate);
    }

    if (is_in) {
        if (size == 1) {
            g_guest_regs.rax = (g_guest_regs.rax & ~0xFFULL) | (result & 0xFF);
        } else if (size == 2) {
            g_guest_regs.rax = (g_guest_regs.rax & ~0xFFFFULL) | (result & 0xFFFF);
        } else {
            g_guest_regs.rax = result & 0xFFFFFFFFULL;  /* 32 位写清零高 32 位 */
        }
    }

    if (terminate) {
        *out_resume = 0;
        return 0;
    }

    vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
    *out_resume = 1;
    return 0;
}

/* ===== EXIT_CPUID 处理：host passthrough + 屏蔽 VMX/hypervisor =====
 *
 * CPUID 在 VMX non-root 无条件 exit。必须真实执行并把结果写回 guest
 * rax/rbx/rcx/rdx，否则 guest 特性探测全错（原"skip"实现会崩 Linux）。
 * leaf 1 ecx：清 bit5(VMX) 与 bit31(hypervisor)，让 guest 认为自己在裸机。
 */
static void cpuid_host(u32 a, u32 c, u32 *oa, u32 *ob, u32 *oc, u32 *od) {
    __asm__ volatile("cpuid"
                     : "=a"(*oa), "=b"(*ob), "=c"(*oc), "=d"(*od)
                     : "a"(a), "c"(c));
}

static int handle_cpuid(u64 rip, u64 instr_len, int *out_resume) {
    u32 a = (u32)g_guest_regs.rax;
    u32 c = (u32)g_guest_regs.rcx;
    u32 oa, ob, oc, od;
    cpuid_host(a, c, &oa, &ob, &oc, &od);

    if (a == 1) {
        oc &= ~(1u << 5);   /* VMX */
        oc &= ~(1u << 31);  /* hypervisor present */
    }
    /* leaf 0x15/0x16 passthrough：QEMU 提供 TSC/总线频率，
     * guest 据此校准 TSC（绕过 PIT ch2 校准路径）。 */

    g_guest_regs.rax = oa;
    g_guest_regs.rbx = ob;
    g_guest_regs.rcx = oc;
    g_guest_regs.rdx = od;

    vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
    *out_resume = 1;
    return 0;
}

/* ===== resume 前例行工作：RX 轮询 + PIT tick + 注入 pending IRQ ===== */

static void poll_host_serial_to_guest(void) {
    for (int i = 0; i < 32; i++) {
        int b = serial_try_read();
        if (b < 0) break;
        int next = (g_rx_tail + 1) % GUEST_RX_SIZE;
        if (next == g_rx_head) break;  /* ring 满 */
        g_guest_rx[g_rx_tail] = (u8)b;
        g_rx_tail = next;
    }
    if (g_rx_head != g_rx_tail) {
        vmx_guest_queue_irq(0x34);  /* COM1 = IRQ4 → vector 0x34 */
    }
}

static void pit_tick_update(void) {
    u64 now = rdtsc_local();
    if (g_last_tick_tsc == 0) {
        g_last_tick_tsc = now;
        if (g_pit_period_tsc == 0) pit_recompute_period();
        return;
    }
    u64 delta = now - g_last_tick_tsc;
    g_last_tick_tsc = now;
    if (g_pit_period_tsc == 0) return;
    g_pit_accum += delta;
    int n = 0;
    while (g_pit_accum >= g_pit_period_tsc && n < 8) {
        g_pit_accum -= g_pit_period_tsc;
        vmx_guest_queue_irq(0x30);  /* PIT = IRQ0 → vector 0x30（自动合并） */
        n++;
    }
    if (g_pit_accum >= g_pit_period_tsc) g_pit_accum = 0;  /* 防积压 */
}

static u64 g_preemption_quantum = 0;

u64 vmx_preemption_quantum_1ms(void) {
    if (g_preemption_quantum) return g_preemption_quantum;
    u64 basic = vmx_read_msr(IA32_VMX_BASIC);
    u64 scale = (basic >> 48) & 0xFF;   /* preemption timer scale */
    if (scale == 0) scale = 1;
    u64 tpm = serial_tsc_per_ms();
    if (tpm == 0) tpm = 2000000;
    g_preemption_quantum = tpm / scale;
    if (g_preemption_quantum == 0) g_preemption_quantum = 1;
    return g_preemption_quantum;
}

int vmx_preemption_timer_supported(void) {
    u64 msr = vmx_read_msr(IA32_VMX_TRUE_PINBASED_CTLS);
    return (msr & (1ULL << (6 + 32))) != 0;  /* bit6 允许置 1 */
}

/* 每次 vmresume 前调用（vmexit_dispatch resume=1 路径末尾）。
 * 重新 arm preemption timer（1ms 周期 exit），轮询 host 串口 RX，
 * 推进 PIT tick，注入 pending IRQ。 */
static void vmexit_before_resume(void) {
    if (!g_linux_guest_active) return;
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum_1ms());
    poll_host_serial_to_guest();
    pit_tick_update();
    maybe_inject_irq();
}

/* ===== EPT violation: 指令解码（用于 virtio-mmio MMIO 数据） =====
 *
 * virtio-mmio 寄存器访问是普通 MOV 指令，如:
 *   mov eax, [gpa]     读  (8A /r 或 A1)
 *   mov [gpa], eax     写  (89 /r 或 A3)
 *   movzx/movsx 等
 *
 * 我们不需要完整解码——只需确定:
 *   1. 访问方向（读 or 写）
 *   2. 访问宽度（1/2/4/8 字节）
 *   3. 写访问时的写入数据；读访问时数据写到哪个 guest GPR
 *
 * 这通过解码指令字节 + 读 guest RIP 处的机器码完成。
 * 用 GVA=EPT GPA 简化：guest 采用恒等页表，GUEST_LINEAR_ADDR 即 GPA。
 */

/* 从 guest 内存读指令字节（GPA=GVA 恒等映射，经 EPT 翻译到 HPA）。 */
static int guest_fetch(u64 gva, u8 *buf, u64 len) {
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    for (u64 i = 0; i < len; i++) {
        u64 hpa = ept_gpa_to_hpa(gva + i);
        if (hpa == 0) return -1;
        buf[i] = *(volatile u8 *)(hhdm + hpa);
    }
    return 0;
}

/* 解码 MMIO 访问指令。
 *   rip_gpa : guest RIP（GPA，恒等映射）
 *   is_write_out : 输出 1=写 MMIO，0=读 MMIO
 *   width_out    : 输出访问宽度（字节）
 *   reg_field_out: 输出目标/源 GPR 在 g_guest_regs 中的索引（0-15）
 * 返回指令总长度（用于回退时推进 RIP），失败返回 0。 */
static u64 decode_mmio_insn(u64 rip_gpa, int *is_write_out, int *width_out,
                            int *reg_field_out) {
    u8 insn[16];
    if (guest_fetch(rip_gpa, insn, sizeof(insn)) != 0) return 0;

    u64 off = 0;
    int rex_w = 0, rex_r = 0;

    /* 跳过前缀 + REX */
    for (;;) {
        u8 b = insn[off];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            (b >= 0x2E && b <= 0x3E && (b==0x2E||b==0x36||b==0x3E||b==0x26||b==0x64||b==0x65))) {
            off++;
            continue;
        }
        if (b >= 0x40 && b <= 0x4F) {  /* REX */
            rex_w = (b >> 3) & 1;
            rex_r = (b >> 2) & 1;
            off++;
        }
        break;
    }

    u8 op = insn[off];
    u8 modrm = insn[off + 1];
    int reg = ((modrm >> 3) & 7) | (rex_r << 3);

    /* mov r, m（读 MMIO）: 8A(8位) / 8B(全宽) */
    if (op == 0x8A || op == 0x8B) {
        *is_write_out = 0;
        *width_out = (op == 0x8A) ? 1 : (rex_w ? 8 : 4);
        *reg_field_out = reg;
        return off + 2;  /* 简化：假设 ModRM mod=00,rm=101(无 SIB/disp) */
    }
    /* mov m, r（写 MMIO）: 88(8位) / 89(全宽) */
    if (op == 0x88 || op == 0x89) {
        *is_write_out = 1;
        *width_out = (op == 0x88) ? 1 : (rex_w ? 8 : 4);
        *reg_field_out = reg;
        return off + 2;
    }
    /* movzx: 0F B6(8→宽) / 0F B7(16→宽) 读 */
    if (op == 0x0F && (insn[off+1] == 0xB6 || insn[off+1] == 0xB7)) {
        *is_write_out = 0;
        *width_out = (insn[off+1] == 0xB6) ? 1 : 2;
        u8 m2 = insn[off + 2];
        *reg_field_out = ((m2 >> 3) & 7) | (rex_r << 3);
        return off + 3;
    }

    /* 无法解码（如带 SIB/displacement 的复杂寻址）。
     * virtio-mmio 驱动用简单寄存器访问，暂不支持则返回 0。 */
    return 0;
}

/* 按索引读写 guest GPR（用于 MMIO 数据的源/目的）。 */
static u64 *gpr_ptr(int idx) {
    switch (idx & 15) {
    case 0: return (u64 *)&g_guest_regs.rax;
    case 1: return (u64 *)&g_guest_regs.rcx;
    case 2: return (u64 *)&g_guest_regs.rdx;
    case 3: return (u64 *)&g_guest_regs.rbx;
    case 6: return (u64 *)&g_guest_regs.rsi;
    case 7: return (u64 *)&g_guest_regs.rdi;
    case 5: return (u64 *)&g_guest_regs.rbp;
    case 4: return (u64 *)&g_guest_regs.rax;  /* rsp 不用作 MMIO 数据，占位 */
    case 8: return (u64 *)&g_guest_regs.r8;
    case 9: return (u64 *)&g_guest_regs.r9;
    case 10: return (u64 *)&g_guest_regs.r10;
    case 11: return (u64 *)&g_guest_regs.r11;
    case 12: return (u64 *)&g_guest_regs.r12;
    case 13: return (u64 *)&g_guest_regs.r13;
    case 14: return (u64 *)&g_guest_regs.r14;
    case 15: return (u64 *)&g_guest_regs.r15;
    }
    return (u64 *)&g_guest_regs.rax;
}

/* 处理 virtio-mmio 设备 GPA 的访问：路由到设备模拟。返回 1=已处理（resume）。 */
static int handle_virtio_mmio_access(u64 gpa, u64 rip, int *out_resume) {
    int is_write, width, reg;
    u64 insn_len = decode_mmio_insn(rip, &is_write, &width, &reg);
    if (insn_len == 0) {
        log_error("[VMEXIT] virtio-mmio: cannot decode access insn");
        log_hex64("[VMEXIT] rip=", rip);
        *out_resume = 0;
        return -1;
    }

    u64 value = 0;
    if (is_write) {
        /* 从 guest GPR 取写入数据（按宽度截断） */
        u64 raw = *gpr_ptr(reg);
        if (width == 1) value = raw & 0xFF;
        else if (width == 2) value = raw & 0xFFFF;
        else if (width == 4) value = raw & 0xFFFFFFFFULL;
        else value = raw;
        virtio_mmio_write(gpa, value, width);
    } else {
        /* 从设备读，写入 guest GPR（按宽度） */
        value = virtio_mmio_read(gpa, width);
        u64 *dst = gpr_ptr(reg);
        if (width == 1) *dst = (*dst & ~0xFFULL) | (value & 0xFF);
        else if (width == 2) *dst = (*dst & ~0xFFFFULL) | (value & 0xFFFF);
        else if (width == 4) *dst = value & 0xFFFFFFFFULL;  /* 32位写清零高32 */
        else *dst = value;
    }

    /* 推进 RIP 越过 MMIO 指令，resume guest */
    vmx_vmcs_write(VMCS_GUEST_RIP, rip + insn_len);
    *out_resume = 1;
    return 0;
}

/* 处理 EPT violation：区分 guest RAM（按需分配）与 virtio-mmio（设备模拟）。
 *
 * exit qualification bits:
 *   bit0 = read access, bit1 = write access, bit2 = instruction fetch
 *   bit3 = EPT entry readable, bit4 = writable, bit5 = executable
 * virtio-mmio GPA 故意不映射 → bit3/4/5 全 0 → "访问不存在页"。 */
static int handle_ept_violation(u64 qualification, u64 rip, int *out_resume) {
    (void)qualification;  /* virtio-mmio 判定只看 GPA，不看访问类型 */
    u64 gpa = vmx_vmcs_read(VMCS_GUEST_PHYSICAL_ADDR);

    /* virtio-mmio 设备区：路由到设备模拟，绝不映射真实内存 */
    if (IS_VIRTIO_MMIO_GPA(gpa)) {
        return handle_virtio_mmio_access(gpa, rip, out_resume);
    }

    /* 普通 guest RAM：按需分配 HPA 页并映射 */
    u64 page = gpa & ~(EPT_PAGE_SIZE - 1);
    if (ept_map_guest_ram_page(page) != 0) {
        log_error("[VMEXIT] EPT violation: guest RAM map failed");
        log_hex64("[VMEXIT] gpa=", gpa);
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
        handle_ept_violation(qualification, rip, &resume);
        break;
    case EXIT_EPT_MISCONFIG:
        /* MMIO 区域权限配置错误或硬件 EPT 表损坏。诊断后终止。 */
        log_error("[VMEXIT] EPT misconfig");
        log_hex64("[VMEXIT] gpa=", vmx_vmcs_read(VMCS_GUEST_PHYSICAL_ADDR));
        resume = 0;
        break;
    case EXIT_EXCEPTION_NMI:
        handle_exception(qualification, rip, &resume);
        break;
    case EXIT_TRIPLE_FAULT:
        log_error("[VMEXIT] triple fault");
        resume = 0;
        break;
    case EXIT_CPUID:
        /* host passthrough + 屏蔽 VMX/hypervisor 位（handle_cpuid 内推进 RIP） */
        handle_cpuid(rip, instr_len, &resume);
        break;
    case EXIT_IO_INSTRUCTION:
        /* legacy 设备模拟：COM1 UART / PIC / PIT / 其余端口吞掉 */
        handle_io(qualification, rip, instr_len, &resume);
        break;
    case EXIT_INTERRUPT_WINDOW:
        /* guest 刚开中断（RFLAGS.IF=1）：尝试注入 pending IRQ */
        {
            u64 cpu = vmx_vmcs_read(VMCS_CPU_BASED_VM_EXEC_CONTROL);
            vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL,
                           cpu & ~CPU_BASED_INTR_WINDOW_EXITING);
            maybe_inject_irq();
            resume = 1;
        }
        break;
    case EXIT_VMX_PREEMPTION_TIMER:
        /* 1ms 周期 exit：resume 前由 vmexit_before_resume 统一
         * 轮询 host 串口 RX / 推进 PIT tick / 注入 pending IRQ */
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

    /* resume 前例行工作：arm preemption timer + host 串口 RX 轮询 +
     * PIT tick 推进 + pending IRQ 注入（仅 Linux guest active 时） */
    if (resume) {
        vmexit_before_resume();
    }
    return resume;
}

u64 vmexit_get_count(void) { return g_vmexit_count; }
