#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/vmm.h>
#include <utsm/hypercall.h>
#include <utsm/ipc_shm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/linux_loader.h>
#include <utsm/linux_xsave.h>
#include <utsm/virtio_mmio.h>
#include <utsm/instr.h>
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
extern void outb(u16 port, u8 value);
extern u8 inb(u16 port);

extern void vmx_vmresume_wrapper(void);

/* serial.c 导出的 host 串口与 TSC 频率接口 */
extern int  serial_try_read(void);
extern u64  serial_tsc_per_ms(void);
extern void serial_putc(char c);

static u64 g_vmexit_count;
static u64 g_linux_tracked_cr3;
#define CR3_RING 16
static u64 g_cr3_ring[CR3_RING];
static u32 g_cr3_ring_n;

/* ---- 插桩: VM-Exit 统计计数器 (热路径，仅 STAT_INC) ---- */
INSTR_STAT_DECL(vmexit_total);
INSTR_STAT_DECL(ept_violation);
INSTR_STAT_DECL(ext_irq);
INSTR_STAT_DECL(cpuid_exit);
INSTR_STAT_DECL(io_exit);
INSTR_STAT_DECL(intr_window);
INSTR_STAT_DECL(preempt_timer);
INSTR_STAT_DECL(msr_exit);
INSTR_STAT_DECL(pre_resume);

static void log_exit_diagnostics(u64 reason, u64 qualification, u64 rip, u64 len) {
    log_hex64("[VMEXIT] reason=", reason);
    log_hex64("[VMEXIT] qual=", qualification);
    log_hex64("[VMEXIT] rip=", rip);
    log_hex64("[VMEXIT] ilen=", len);
}

/* 处理 HLT：
 * - self-test guest：终止（guest 完成测试）
 * - Linux guest idle：推进 RIP 并留在 guest，等 PIT/virtio IRQ。
 *   daemon PARK 走 UTSM_HCALL_PARK，不再用 HLT（idle=poll 会在
 *   cpu_idle_poll 里把 current 读成 NULL 并 panic）。 */
static int handle_hlt(u64 rip, u64 instr_len, int *out_resume) {
    *out_resume = 0;   /* 三种情况默认 exit-to-host */
    if (g_linux_guest_active) {
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        *out_resume = 1;
        return 0;
    } else if (g_xj380_guest_active) {
        /* OpenXJ380 guest: 推进 RIP 并立即继续执行（不 park 不 terminate） */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        *out_resume = 1;
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
static u8  g_pic_vec_base = 0x30;   /* ICW2；x86_64 Linux ISA 向量基址 */
static int g_pic_icw_state = 0;     /* 0=就绪(OCW), 1=等ICW2, 2=等ICW3, 3=等ICW4 */
static int g_pic_icw_sngl = 0;
static int g_pic_icw_need4 = 0;
static u32 g_irq_skip_logs;
static u32 g_irq_inject_logs;

#define GUEST_INTR_STI   (1ULL << 0)
#define GUEST_INTR_MOVSS (1ULL << 1)

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

    /* IMR：vector = ICW2 基址 + irq。基址未写入前按 0x30。 */
    if (vec >= g_pic_vec_base && vec < (u8)(g_pic_vec_base + 8)) {
        if (g_pic_imr & (1u << (vec - g_pic_vec_base))) {
            /* Keep the IRQ queued. virtio-mmio is edge-like: dropping it
             * here loses the completion forever if the guest parked while
             * the line was still masked. */
            if (g_irq_skip_logs < 8) {
                g_irq_skip_logs++;
                log_hex64("[IRQ] masked imr=", g_pic_imr);
                log_hex64("[IRQ] masked vec=", vec);
            }
            return;
        }
    }

    u64 rflags = vmx_vmcs_read(VMCS_GUEST_RFLAGS);
    u64 intr_st = vmx_vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
    u64 cpu = vmx_vmcs_read(VMCS_CPU_BASED_VM_EXEC_CONTROL);

    /* External IRQ may be injected while virtual-NMI blocking is set.
     * Only STI-shadow and MOV-SS-shadow must delay delivery. Requiring
     * interruptibility==0 froze PIT after park: nanosleep never woke. */
    if ((rflags & 0x200) && !(intr_st & (GUEST_INTR_STI | GUEST_INTR_MOVSS))) {
        vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO,
                       VM_ENTRY_INTR_INFO_VALID |
                       (VM_ENTRY_INTR_TYPE_HW_IRQ << 8) | vec);
        g_pending_head = (g_pending_head + 1) % IRQ_PENDING_MAX;
        if (cpu & CPU_BASED_INTR_WINDOW_EXITING) {
            vmx_vmcs_write(VMCS_CPU_BASED_VM_EXEC_CONTROL,
                           cpu & ~CPU_BASED_INTR_WINDOW_EXITING);
        }
        if (g_irq_inject_logs < 4) {
            g_irq_inject_logs++;
            log_hex64("[IRQ] inject vec=", vec);
            log_hex64("[IRQ] inject base=", g_pic_vec_base);
            log_hex64("[IRQ] inject intr=", intr_st);
        }
    } else {
        if (g_irq_skip_logs < 8) {
            g_irq_skip_logs++;
            log_hex64("[IRQ] window if/sti rflags=", rflags);
            log_hex64("[IRQ] window intr=", intr_st);
        }
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
        else {
            /* Direct host UART write. serial_putc() waits on the same
             * COM1 LSR and can stall guest printk under file-backed serial. */
            char c = (char)(value & 0xFF);
            if (c == '\n') {
                for (int i = 0; i < 10000 && !(inb(0x3F8 + 5) & 0x20); i++) {}
                outb(0x3F8, '\r');
            }
            for (int i = 0; i < 10000 && !(inb(0x3F8 + 5) & 0x20); i++) {}
            outb(0x3F8, (u8)c);
        }
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
        case 1:  /* ICW2 向量基址（低 3 位忽略） */
            g_pic_vec_base = (u8)value & 0xF8u;
            if (g_pic_vec_base == 0)
                g_pic_vec_base = 0x30;
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
            maybe_inject_irq();
            break;
        }
        return 0;
    }
    /* 8259 PIC slave：吞掉（不用） */
    if (port == 0xA0 || port == 0xA1) return is_in ? 0xFF : 0;

    /* 8254 PIT */
    if (port >= 0x40 && port <= 0x43) return pit_io(port, is_in, value);

    /* Port 0x61: NMI/speaker/PIT2. Linux pit_hpet_ptimer_calibrate_cpu
     * waits for bit5 (PIT ch2 OUT). A constant 0 livelocks that loop. */
    if (port == 0x61) {
        static u8 port61_gate;
        if (is_in) {
            port61_gate ^= 0x10;          /* bit4 refresh toggle */
            return (u8)((port61_gate & 0x13) | 0x20); /* bit5 = PIT2 OUT */
        }
        port61_gate = (u8)((port61_gate & ~0x03) | (value & 0x03));
        return 0;
    }

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

    /* Nested KVM sometimes reports VM-exit instruction length 0 for IN/OUT.
     * Adding 0 leaves RIP on the same opcode and livelocks extract_kernel
     * earlyprintk (millions of exits at one RIP). Immediate-port encodings
     * are 2 bytes; DX-port encodings are 1. */
    if (instr_len == 0) {
        instr_len = ((qualification >> 6) & 1) ? 2 : 1;
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
        oc &= ~(1u << 17);  /* PCID: keep Linux on MOV-CR3 TLB flush */
        oc &= ~(1u << 12);  /* FMA: requires AVX */
        oc &= ~(1u << 29);  /* F16C */
        oc &= ~(1u << 31);  /* hypervisor present */
    }
    linux_xsave_adjust_cpuid(a, c, &oa, &ob, &oc, &od,
                             vmx_vmcs_read(VMCS_GUEST_CR4));
    if (a == 7 && c != 0) {
        /* Hide CET_SSS and other 7.1+ bits; Linux 6.6 probes subleaf 1. */
        oa = 0;
        ob = 0;
        oc = 0;
        od = 0;
    }
    if (a == 7 && c == 0) {
        /* Nested KVM: these insns #UD in the guest even when host CPUID
         * advertises them. INVPCID is used by __flush_tlb_all. */
        ob &= ~(1u << 0);    /* FSGSBASE */
        ob &= ~(1u << 5);    /* AVX2: keep IFUNC on SSE until YMM save is on */
        ob &= ~(1u << 10);   /* INVPCID */
        ob &= ~(1u << 16);   /* AVX512F */
        ob &= ~(1u << 17);   /* AVX512DQ */
        ob &= ~(1u << 21);   /* AVX512IFMA */
        ob &= ~(1u << 26);   /* AVX512PF */
        ob &= ~(1u << 27);   /* AVX512ER */
        ob &= ~(1u << 28);   /* AVX512CD */
        ob &= ~(1u << 30);   /* AVX512BW */
        ob &= ~(1u << 31);   /* AVX512VL */
        oc &= ~(1u << 1);    /* AVX512VBMI */
        oc &= ~(1u << 5);    /* WAITPKG / TPAUSE */
        oc &= ~(1u << 6);    /* AVX512VBMI2 */
        oc &= ~(1u << 7);    /* CET_SS: SSP=0 makes PUSH fault at address 0 */
        oc &= ~(1u << 11);   /* AVX512VNNI */
        oc &= ~(1u << 12);   /* AVX512BITALG */
        oc &= ~(1u << 14);   /* AVX512VPOPCNTDQ */
        oc &= ~(1u << 16);   /* LA57 */
        od &= ~(1u << 2);    /* AVX5124VNNIW */
        od &= ~(1u << 3);    /* AVX5124FMAPS */
        od &= ~(1u << 8);    /* AVX512VP2INTERSECT */
        od &= ~(1u << 20);   /* CET_IBT */
        od &= ~(1u << 23);   /* AVX512FP16 */
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
        vmx_guest_queue_irq(g_pic_vec_base);  /* PIT = IRQ0 */
        n++;
    }
    if (g_pit_accum >= g_pit_period_tsc) g_pit_accum = 0;  /* 防积压 */
}

static u64 g_preemption_quantum = 0;
#define VMX_PREEMPTION_INTERVAL_MS 10ULL
/* ~80ms of guest time, then return to DSK so blit/Esc can run without
 * requiring the exec daemon to HLT (HLT freezes virtio completions). */
#define LINUX_HOST_SLICE_TICKS 8u
static unsigned g_linux_slice_ticks;
static int g_linux_timeslice_armed;
static void vmexit_before_resume(void);

void vmx_linux_timeslice_reset(void) {
    g_linux_slice_ticks = 0;
    g_linux_timeslice_armed = 1;
}

void vmx_linux_timeslice_disarm(void) {
    g_linux_timeslice_armed = 0;
}

void vmx_linux_prepare_entry(void) {
    vmexit_before_resume();
}

u64 vmx_preemption_quantum(void) {
    if (g_preemption_quantum) return g_preemption_quantum;
    /* IA32_VMX_MISC[4:0] is the VMX-preemption-timer rate.  The timer
     * decrements once per 2^rate TSC ticks; IA32_VMX_BASIC[55:48] is not
     * this rate (it contains unrelated VMX-basic capabilities). */
    u64 misc = vmx_read_msr(IA32_VMX_MISC);
    u64 rate = misc & 0x1F;
    u64 tpm = serial_tsc_per_ms();
    if (tpm == 0) tpm = 2000000;
    g_preemption_quantum =
        (tpm * VMX_PREEMPTION_INTERVAL_MS) >> rate;
    if (g_preemption_quantum == 0) g_preemption_quantum = 1;
    log_hex64("[VMX] preemption timer rate=", rate);
    log_hex64("[VMX] preemption timer interval_ms=",
              VMX_PREEMPTION_INTERVAL_MS);
    log_hex64("[VMX] preemption timer quantum=", g_preemption_quantum);
    return g_preemption_quantum;
}

int vmx_preemption_timer_supported(void) {
    u64 msr = vmx_read_msr(IA32_VMX_TRUE_PINBASED_CTLS);
    return (msr & (1ULL << (6 + 32))) != 0;  /* bit6 允许置 1 */
}

/* 每次 vmresume 前调用（vmexit_dispatch resume=1 路径末尾）。
 * 重新 arm preemption timer（1ms 周期 exit），轮询 host 串口 RX，
 * 推进 PIT tick，注入 pending IRQ。
 * 同时轮询 virtio-net RX：guest 投满 RX buffer 后不再 notify，
 * host 网卡收到的包靠这里周期性填充并注入 IRQ6。 */
static void linux_clear_guest_cet(void) {
    u64 cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    if (cr4 & CR4_CET) {
        vmx_vmcs_write(VMCS_GUEST_CR4, cr4 & ~CR4_CET);
    }
}

static u64 g_pf_cr2;
static int g_pf_need_cr2;

static void write_cr2_local(u64 v) {
    __asm__ volatile("mov %0, %%cr2" :: "r"(v));
}

static void vmexit_before_resume(void) {
    if (!g_linux_guest_active) return;
    if (g_pf_need_cr2) {
        write_cr2_local(g_pf_cr2);
        g_pf_need_cr2 = 0;
    }
    linux_clear_guest_cet();
    linux_cet_vmcs_sync_host();
    linux_cet_force_guest_off();
    vmx_vmcs_write(VMCS_GUEST_PREEMPTION_TIMER, vmx_preemption_quantum());
    poll_host_serial_to_guest();
    virtio_net_poll();
    virtio_input_poll();   /* VSCode Phase 2: drain pending kbd/mouse events */
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

/* Guest GVA → GPA via a CR3 value (GPA of the PGD). Linux PTI uses two
 * adjacent 4K PGDs; user and kernel CR3 differ by bit 12. */
static u64 guest_walk_cr3_at(u64 gva, u64 cr3, int five_level) {
    int first = five_level ? 0 : 1;
    int shifts[5] = { 48, 39, 30, 21, 12 };
    u64 table_gpa = cr3 & 0x000FFFFFFFFFF000ULL;
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    if (!hhdm || !table_gpa) return 0;

    for (int level = first; level < 5; level++) {
        u64 table_hpa = ept_gpa_to_hpa(table_gpa);
        if (!table_hpa) return 0;
        u64 *table = (u64 *)(hhdm + table_hpa);
        u64 entry = table[(gva >> shifts[level]) & 0x1FF];
        if (!(entry & 1)) return 0;
        if (shifts[level] == 30 && (entry & (1ULL << 7))) {
            return (entry & 0x000FFFFFC0000000ULL) + (gva & 0x3FFFFFFFULL);
        }
        if (shifts[level] == 21 && (entry & (1ULL << 7))) {
            return (entry & 0x000FFFFFFFE00000ULL) + (gva & 0x1FFFFFULL);
        }
        table_gpa = entry & 0x000FFFFFFFFFF000ULL;
    }
    return table_gpa + (gva & 0xFFFULL);
}

static u64 guest_current_cr3(void) {
    if (g_linux_tracked_cr3)
        return g_linux_tracked_cr3;
    return vmx_vmcs_read(VMCS_GUEST_CR3);
}

static int bytes_nonzero(const u8 *buf, u64 len) {
    u64 i;
    for (i = 0; i < len; i++) {
        if (buf[i]) return 1;
    }
    return 0;
}

static void track_guest_cr3(u64 val) {
    u32 i;
    g_linux_tracked_cr3 = val;
    vmx_vmcs_write(VMCS_GUEST_CR3, val);
    for (i = 0; i < g_cr3_ring_n && i < CR3_RING; i++) {
        if (g_cr3_ring[i] == val)
            return;
    }
    if (g_cr3_ring_n < CR3_RING) {
        g_cr3_ring[g_cr3_ring_n++] = val;
    } else {
        for (i = 1; i < CR3_RING; i++)
            g_cr3_ring[i - 1] = g_cr3_ring[i];
        g_cr3_ring[CR3_RING - 1] = val;
    }
    /* MOV CR3 did not execute; flush the guest VPID so L2 does not keep
     * user translations from the previous mm. */
    ept_flush_vpid(2);
}

static u64 guest_walk_cr3(u64 gva, int five_level) {
    u64 cr3 = guest_current_cr3();
    u64 gpa = guest_walk_cr3_at(gva, cr3, five_level);
    if (gpa) return gpa;
    /* PTI: the other PGD is the adjacent 4K page. */
    return guest_walk_cr3_at(gva, cr3 ^ 0x1000ULL, five_level);
}

u64 vmx_guest_gva_to_gpa(u64 gva) {
    u64 cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    int la57 = (cr4 & (1ULL << 12)) ? 1 : 0;
    u64 gpa = guest_walk_cr3(gva, la57);
    if (gpa) return gpa;
    /* Linux 6.x L4 direct map. Kernel stacks live here after paging_init. */
    if (gva >= 0xFFFF888000000000ULL && gva < 0xFFFFC88000000000ULL) {
        gpa = gva - 0xFFFF888000000000ULL;
        if (ept_gpa_to_hpa(gpa)) return gpa;
    }
    /* Retry the other paging mode if CR4.LA57 does not match the tables. */
    gpa = guest_walk_cr3(gva, !la57);
    if (gpa) return gpa;
    if (ept_gpa_to_hpa(gva)) return gva;
    return 0;
}

/* 从 guest 内存读指令字节（GVA→GPA→HPA）。
 * 若当前 CR3 走到全 0 页（PTI 走错 PGD / zeropage），改走 CR3^0x1000。 */
static int guest_fetch_at_cr3(u64 gva, u8 *buf, u64 len, u64 cr3) {
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    u64 cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    int la57 = (cr4 & (1ULL << 12)) ? 1 : 0;
    if (!hhdm) return -1;
    for (u64 i = 0; i < len; i++) {
        u64 gpa = guest_walk_cr3_at(gva + i, cr3, la57);
        if (!gpa) gpa = guest_walk_cr3_at(gva + i, cr3, !la57);
        u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
        if (hpa == 0) return -1;
        buf[i] = *(volatile u8 *)(hhdm + hpa);
    }
    return 0;
}

static int guest_fetch(u64 gva, u8 *buf, u64 len) {
    u64 cr3s[CR3_RING + 4];
    u32 n = 0, i, j;
    u64 cur = guest_current_cr3();
    u64 vmcs = vmx_vmcs_read(VMCS_GUEST_CR3);

    cr3s[n++] = cur;
    if (vmcs != cur)
        cr3s[n++] = vmcs;
    cr3s[n++] = cur ^ 0x1000ULL;
    for (i = g_cr3_ring_n; i > 0; i--) {
        int dup = 0;
        u64 c = g_cr3_ring[i - 1];
        for (j = 0; j < n; j++) {
            if (cr3s[j] == c) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            cr3s[n++] = c;
    }
    for (i = 0; i < n; i++) {
        if (guest_fetch_at_cr3(gva, buf, len, cr3s[i]) != 0)
            continue;
        if (bytes_nonzero(buf, len))
            return 0;
    }
    /* Low identity window: PTI walk can hit the zeropage while the
     * CPU still executes the mapped ELF (busybox at 0x400000). */
    if (gva < 0x04000000ULL && len > 0) {
        u64 hhdm = g_hhdm_request.response ?
                   g_hhdm_request.response->offset : 0;
        if (hhdm) {
            int ok = 1;
            for (i = 0; i < len; i++) {
                u64 hpa = ept_gpa_to_hpa(gva + i);
                if (!hpa) {
                    ok = 0;
                    break;
                }
                buf[i] = *(volatile u8 *)(hhdm + hpa);
            }
            if (ok && bytes_nonzero(buf, len))
                return 0;
        }
    }
    return -1;
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
    u64 payload = off + 1;
    if (op == 0x0F) payload = off + 2;

    u8 modrm = insn[payload];
    int mod = (modrm >> 6) & 3;
    int rm = modrm & 7;
    int reg = ((modrm >> 3) & 7) | (rex_r << 3);
    u64 n = payload + 1; /* opcode(s) + ModRM */
    if (mod != 3 && rm == 4) {
        u8 sib = insn[n];
        n += 1;
        if (mod == 0 && (sib & 7) == 5) n += 4;
    }
    if (mod == 1) n += 1;
    else if (mod == 2) n += 4;
    else if (mod == 0 && rm == 5) n += 4;

    /* mov r, m（读 MMIO）: 8A(8位) / 8B(全宽) */
    if (op == 0x8A || op == 0x8B) {
        *is_write_out = 0;
        *width_out = (op == 0x8A) ? 1 : (rex_w ? 8 : 4);
        *reg_field_out = reg;
        return n;
    }
    /* mov m, r（写 MMIO）: 88(8位) / 89(全宽) */
    if (op == 0x88 || op == 0x89) {
        *is_write_out = 1;
        *width_out = (op == 0x88) ? 1 : (rex_w ? 8 : 4);
        *reg_field_out = reg;
        return n;
    }
    /* movzx: 0F B6(8→宽) / 0F B7(16→宽) 读 */
    if (op == 0x0F && (insn[off+1] == 0xB6 || insn[off+1] == 0xB7)) {
        *is_write_out = 0;
        *width_out = (insn[off+1] == 0xB6) ? 1 : 2;
        *reg_field_out = reg;
        return n;
    }
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

static u64 read_gpr_full(int idx) {
    if ((idx & 15) == 4)
        return vmx_vmcs_read(VMCS_GUEST_RSP);
    return *gpr_ptr(idx);
}

static void write_gpr_full(int idx, u64 v) {
    if ((idx & 15) == 4)
        vmx_vmcs_write(VMCS_GUEST_RSP, v);
    else
        *gpr_ptr(idx) = v;
}

static int guest_write_u64(u64 gva, u64 value) {
    u64 hhdm = g_hhdm_request.response ? g_hhdm_request.response->offset : 0;
    u8 bytes[8];
    u64 i;
    if (!hhdm) return -1;
    for (i = 0; i < 8; i++)
        bytes[i] = (u8)((value >> (i * 8)) & 0xFF);
    for (i = 0; i < 8; i++) {
        u64 gpa = vmx_guest_gva_to_gpa(gva + i);
        u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
        if (!hpa && gpa) {
            ept_map_guest_ram_page(gpa & ~(EPT_PAGE_SIZE - 1));
            hpa = ept_gpa_to_hpa(gpa);
        }
        if (!hpa) return -1;
        *(volatile u8 *)(hhdm + hpa) = bytes[i];
    }
    return 0;
}

static int guest_read_u64(u64 gva, u64 *out) {
    u8 bytes[8];
    u64 i, v = 0;
    if (guest_fetch(gva, bytes, 8) != 0) return -1;
    for (i = 0; i < 8; i++)
        v |= ((u64)bytes[i]) << (i * 8);
    *out = v;
    return 0;
}

static int guest_is_user_rip(u64 rip) {
    return (rip & (1ULL << 63)) == 0;
}

static u64 read_cr2_local(void) {
    u64 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* Nested KVM leaks host SHSTK: CALL/RET/PUSH fault at SSP=0 (GVA 0).
 * Complete the data-stack side and skip the shadow-stack store. */
static int cet_emulate_shstk_insn(u64 rip, u64 *ilen_hint) {
    u8 insn[16];
    u64 off = 0;
    int rex_b = 0;
    u64 rsp, next;
    static u32 emu_logs;

    if (guest_fetch(rip, insn, sizeof(insn)) != 0)
        return -1;
    if (insn[0] == 0 && insn[1] == 0 && insn[2] == 0 && insn[3] == 0)
        return -1;

    while (off < 4 && ((insn[off] >= 0x40 && insn[off] <= 0x4F))) {
        rex_b = insn[off] & 1;
        off++;
    }
    if (off >= sizeof(insn)) return -1;

    rsp = vmx_vmcs_read(VMCS_GUEST_RSP);

    /* PUSH r64: 50+rd */
    if (insn[off] >= 0x50 && insn[off] <= 0x57) {
        int rd = (insn[off] - 0x50) | (rex_b << 3);
        u64 val = read_gpr_full(rd);
        rsp -= 8;
        if (guest_write_u64(rsp, val) != 0) return -1;
        vmx_vmcs_write(VMCS_GUEST_RSP, rsp);
        next = rip + off + 1;
        vmx_vmcs_write(VMCS_GUEST_RIP, next);
        if (emu_logs < 12) {
            log_hex64("[VMEXIT] CET emulate PUSH r rip=", rip);
            emu_logs++;
        }
        *ilen_hint = off + 1;
        return 0;
    }
    /* POP r64: 58+rd — data stack only */
    if (insn[off] >= 0x58 && insn[off] <= 0x5F) {
        int rd = (insn[off] - 0x58) | (rex_b << 3);
        u64 val;
        if (guest_read_u64(rsp, &val) != 0) return -1;
        write_gpr_full(rd, val);
        vmx_vmcs_write(VMCS_GUEST_RSP, rsp + 8);
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + off + 1);
        if (emu_logs < 12) {
            log_hex64("[VMEXIT] CET emulate POP r rip=", rip);
            emu_logs++;
        }
        return 0;
    }
    /* CALL rel32 */
    if (insn[off] == 0xE8) {
        u32 rel = (u32)insn[off + 1] | ((u32)insn[off + 2] << 8) |
                  ((u32)insn[off + 3] << 16) | ((u32)insn[off + 4] << 24);
        next = rip + off + 5;
        rsp -= 8;
        if (guest_write_u64(rsp, next) != 0) return -1;
        vmx_vmcs_write(VMCS_GUEST_RSP, rsp);
        vmx_vmcs_write(VMCS_GUEST_RIP, next + (u64)(int)rel);
        if (emu_logs < 12) {
            log_hex64("[VMEXIT] CET emulate CALL rel32 rip=", rip);
            emu_logs++;
        }
        return 0;
    }
    /* RET */
    if (insn[off] == 0xC3) {
        u64 ret;
        if (guest_read_u64(rsp, &ret) != 0) return -1;
        vmx_vmcs_write(VMCS_GUEST_RSP, rsp + 8);
        vmx_vmcs_write(VMCS_GUEST_RIP, ret);
        if (emu_logs < 12) {
            log_hex64("[VMEXIT] CET emulate RET rip=", rip);
            emu_logs++;
        }
        return 0;
    }
    /* CALL/PUSH/JMP r/m64: FF /2, /6, /4 with ModRM.mod==3 */
    if (insn[off] == 0xFF) {
        u8 modrm = insn[off + 1];
        int mod = (modrm >> 6) & 3;
        int reg = (modrm >> 3) & 7;
        int rm = (modrm & 7) | (rex_b << 3);
        if (mod == 3 && (reg == 2 || reg == 6)) {
            u64 tgt = read_gpr_full(rm);
            next = rip + off + 2;
            rsp -= 8;
            if (guest_write_u64(rsp, next) != 0) return -1;
            vmx_vmcs_write(VMCS_GUEST_RSP, rsp);
            if (reg == 2)
                vmx_vmcs_write(VMCS_GUEST_RIP, tgt);
            else
                vmx_vmcs_write(VMCS_GUEST_RIP, next);
            if (emu_logs < 12) {
                log_hex64("[VMEXIT] CET emulate FF r/m rip=", rip);
                emu_logs++;
            }
            return 0;
        }
    }
    if (emu_logs < 12) {
        log_hex64("[VMEXIT] CET emulate miss rip=", rip);
        log_hex64("[VMEXIT] CET emulate miss insn=",
                  ((u64)insn[0]) | ((u64)insn[1] << 8) |
                  ((u64)insn[2] << 16) | ((u64)insn[3] << 24));
        emu_logs++;
    }
    return -1;
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

    /* 普通 guest RAM：按需分配 HPA 页并映射。
     * Nested KVM can deliver spurious EPT violations on an already-mapped
     * GPA. Allocating a fresh page would discard libc/text that Linux
     * already wrote (PTE then points at a zero hole). Upgrade flags. */
    u64 page = gpa & ~(EPT_PAGE_SIZE - 1);
    u64 existing = ept_gpa_to_hpa(page);
    if (existing) {
        static u32 reuse_logs;
        if (ept_map_range(page, existing, EPT_PAGE_SIZE, EPT_RWX) != 0) {
            log_error("[VMEXIT] EPT upgrade existing page failed");
            log_hex64("[VMEXIT] gpa=", gpa);
            *out_resume = 0;
            return -1;
        }
        ept_flush_ept();
        if (reuse_logs < 8) {
            reuse_logs++;
            log_hex64("[VMEXIT] EPT reuse gpa=", gpa);
            log_hex64("[VMEXIT] EPT reuse hpa=", existing);
        }
        *out_resume = 1;
        return 0;
    }
    if (ept_map_guest_ram_page(page) != 0) {
        log_error("[VMEXIT] EPT violation: guest RAM map failed");
        log_hex64("[VMEXIT] gpa=", gpa);
        *out_resume = 0;
        return -1;
    }
    *out_resume = 1;
    return 0;
}

static void log_guest_page_walk(u64 linear) {
    u64 cr3 = guest_current_cr3();
    u64 cr4 = vmx_vmcs_read(VMCS_GUEST_CR4);
    int shifts[5] = { 48, 39, 30, 21, 12 };
    int first = (cr4 & (1ULL << 12)) ? 0 : 1;  /* CR4.LA57 */
    u64 table_gpa = cr3 & 0x000FFFFFFFFFF000ULL;
    u64 hhdm = g_hhdm_request.response ?
               g_hhdm_request.response->offset : 0;

    log_hex64("[VMEXIT] walk cr4=", cr4);
    for (int level = first; level < 5; level++) {
        u64 table_hpa = ept_gpa_to_hpa(table_gpa);
        log_hex64("[VMEXIT] walk table_gpa=", table_gpa);
        log_hex64("[VMEXIT] walk table_hpa=", table_hpa);
        if (!table_hpa || !hhdm) break;

        u64 *table = (u64 *)(hhdm + table_hpa);
        u64 index = (linear >> shifts[level]) & 0x1FF;
        u64 entry = table[index];
        log_hex64("[VMEXIT] walk index=", index);
        log_hex64("[VMEXIT] walk entry=", entry);
        if (!(entry & 1)) break;
        if ((shifts[level] == 30 || shifts[level] == 21) &&
            (entry & (1ULL << 7))) {
            break;
        }
        table_gpa = entry & 0x000FFFFFFFFFF000ULL;
    }
}

/* MOV CR3: keep a software copy. Nested KVM leaves VMCS GUEST_CR3 stuck
 * on an early kernel PGD (zero pages for user GVA). */
static int handle_cr_access(u64 qualification, u64 rip, u64 instr_len,
                            int *out_resume) {
    u32 cr = (u32)(qualification & 0xF);
    u32 type = (u32)((qualification >> 4) & 3);
    u32 reg = (u32)((qualification >> 8) & 0xF);
    static u32 logs;

    if (cr == 3 && type == 0) {
        u64 val = read_gpr_full((int)reg);
        track_guest_cr3(val);
        if (logs < 12) {
            log_hex64("[VMEXIT] MOV CR3=", val);
            logs++;
        }
    } else if (cr == 3 && type == 1) {
        write_gpr_full((int)reg, guest_current_cr3());
    }
    vmx_vmcs_write(VMCS_GUEST_RIP, rip + (instr_len ? instr_len : 3));
    *out_resume = 1;
    return 0;
}

/* 处理通用异常：打印诊断信息，终止 guest。 */
static int handle_exception(u64 qualification, u64 rip, int *out_resume) {
    static u32 g_exc_logs;
    static u32 g_user_exc_logs;
    u64 intr_info = vmx_vmcs_read(VMCS_EXIT_INTR_INFO);
    int verbose = g_exc_logs < 8;
    int user_rip = guest_is_user_rip(rip);
    if (verbose) g_exc_logs++;
    if (verbose) {
        log_hex64("[VMEXIT] exception intr_info=", intr_info);
        if (intr_info & (1ULL << 11)) {
            log_hex64("[VMEXIT] exception error_code=",
                      vmx_vmcs_read(VMCS_EXIT_INTR_ERROR_CODE));
        }
        log_hex64("[VMEXIT] exception gla=",
                  vmx_vmcs_read(VMCS_GUEST_LINEAR_ADDR));
        log_hex64("[VMEXIT] exception qual=", qualification);
        log_hex64("[VMEXIT] exception rip=", rip);
        log_hex64("[VMEXIT] exception cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        log_hex64("[VMEXIT] exception rsp=", vmx_vmcs_read(VMCS_GUEST_RSP));
        log_hex64("[VMEXIT] exception rbx=", g_guest_regs.rbx);
        log_hex64("[VMEXIT] exception rsi=", g_guest_regs.rsi);
        log_hex64("[VMEXIT] exception rdi=", g_guest_regs.rdi);
        log_hex64("[VMEXIT] exception r11=", g_guest_regs.r11);
        log_hex64("[VMEXIT] exception r15=", g_guest_regs.r15);
        {
            u8 insn[8];
            if (guest_fetch(rip, insn, sizeof(insn)) == 0) {
                log_hex64("[VMEXIT] insn0=",
                          ((u64)insn[0]) | ((u64)insn[1] << 8) |
                          ((u64)insn[2] << 16) | ((u64)insn[3] << 24) |
                          ((u64)insn[4] << 32) | ((u64)insn[5] << 40) |
                          ((u64)insn[6] << 48) | ((u64)insn[7] << 56));
            }
        }
        if ((intr_info & 0xFF) == 14) {
            log_guest_page_walk(qualification);
        }
    }
    if (g_linux_guest_active && user_rip && g_user_exc_logs < 16) {
        u8 insn[8];
        g_user_exc_logs++;
        log_hex64("[VMEXIT] user-exc vec=", intr_info & 0xFF);
        log_hex64("[VMEXIT] user-exc err=",
                  (intr_info & (1ULL << 11)) ?
                  vmx_vmcs_read(VMCS_EXIT_INTR_ERROR_CODE) : 0);
        log_hex64("[VMEXIT] user-exc rip=", rip);
        log_hex64("[VMEXIT] user-exc cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        log_hex64("[VMEXIT] user-exc tracked=", guest_current_cr3());
        log_hex64("[VMEXIT] user-exc csar=", vmx_vmcs_read(VMCS_GUEST_CS_ACCESS));
        log_hex64("[VMEXIT] user-exc ilen=",
                  vmx_vmcs_read(VMCS_INSTRUCTION_LENGTH));
        log_hex64("[VMEXIT] user-exc walk=", vmx_guest_gva_to_gpa(rip));
        log_hex64("[VMEXIT] user-exc hpa=",
                  ept_gpa_to_hpa(vmx_guest_gva_to_gpa(rip)));
        {
            u64 gpa = vmx_guest_gva_to_gpa(rip);
            u64 hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
            u64 hhdm = g_hhdm_request.response ?
                       g_hhdm_request.response->offset : 0;
            if (hpa && hhdm) {
                u8 *p = (u8 *)(hhdm + hpa);
                log_hex64("[VMEXIT] user-exc mem=",
                          ((u64)p[0]) | ((u64)p[1] << 8) |
                          ((u64)p[2] << 16) | ((u64)p[3] << 24) |
                          ((u64)p[4] << 32) | ((u64)p[5] << 40) |
                          ((u64)p[6] << 48) | ((u64)p[7] << 56));
            }
            gpa = guest_walk_cr3_at(rip, guest_current_cr3() ^ 0x1000ULL, 0);
            hpa = gpa ? ept_gpa_to_hpa(gpa) : 0;
            log_hex64("[VMEXIT] user-exc xor-gpa=", gpa);
            log_hex64("[VMEXIT] user-exc xor-hpa=", hpa);
            if (hpa && hhdm) {
                u8 *p = (u8 *)(hhdm + hpa);
                log_hex64("[VMEXIT] user-exc xor-mem=",
                          ((u64)p[0]) | ((u64)p[1] << 8) |
                          ((u64)p[2] << 16) | ((u64)p[3] << 24) |
                          ((u64)p[4] << 32) | ((u64)p[5] << 40) |
                          ((u64)p[6] << 48) | ((u64)p[7] << 56));
            }
        }
        if (guest_fetch(rip, insn, sizeof(insn)) == 0) {
            log_hex64("[VMEXIT] user-exc insn0=",
                      ((u64)insn[0]) | ((u64)insn[1] << 8) |
                      ((u64)insn[2] << 16) | ((u64)insn[3] << 24) |
                      ((u64)insn[4] << 32) | ((u64)insn[5] << 40) |
                      ((u64)insn[6] << 48) | ((u64)insn[7] << 56));
        } else {
            log_info("[VMEXIT] user-exc fetch failed");
        }
        if (g_user_exc_logs == 1)
            log_guest_page_walk(rip);
    }
    if ((intr_info & 0xFF) == 8) {
        log_error("[VMEXIT] #DF (will not reinject)");
        log_hex64("[VMEXIT] df cr0=", vmx_vmcs_read(VMCS_GUEST_CR0));
        log_hex64("[VMEXIT] df cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        log_hex64("[VMEXIT] df cr4=", vmx_vmcs_read(VMCS_GUEST_CR4));
        log_hex64("[VMEXIT] df idtr=", vmx_vmcs_read(VMCS_GUEST_IDTR_BASE));
        log_hex64("[VMEXIT] df idtl=", vmx_vmcs_read(VMCS_GUEST_IDTR_LIMIT));
        *out_resume = 0;
        return 0;
    }
    if (g_linux_guest_active) {
        u32 vec = (u32)(intr_info & 0xFF);
        u32 err = 0;
        u64 ilen = vmx_vmcs_read(VMCS_INSTRUCTION_LENGTH);
        if (intr_info & (1ULL << 11))
            err = (u32)vmx_vmcs_read(VMCS_EXIT_INTR_ERROR_CODE);

        /* User #PF at page 0 / wrap: leaked SHSTK store with SSP=0.
         * Always stash CR2 so a reinjected #PF still sees the L2 address
         * (CR2 is not a VMCS field). */
        if (vec == 14) {
            u64 cr2 = read_cr2_local();
            u64 gla = vmx_vmcs_read(VMCS_GUEST_LINEAR_ADDR);
            u64 addr = cr2 ? cr2 : gla;
            g_pf_cr2 = addr;
            g_pf_need_cr2 = 1;
            if (guest_is_user_rip(rip) &&
                (addr < 0x1000ULL || addr >= 0xFFFFFFFFFFFFF000ULL)) {
                u64 dummy = 0;
                if (cet_emulate_shstk_insn(rip, &dummy) == 0) {
                    g_pf_need_cr2 = 0;
                    vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO, 0);
                    *out_resume = 1;
                    return 0;
                }
            }
        }

        /* IBT: skip only when the bytes are ENDBR or the four-NOP strip.
         * A 16-byte-aligned zero-fetch skip desynced utsm_exec_daemon. */
        if ((vec == 13 || vec == 6 || vec == 21) &&
            guest_is_user_rip(rip) &&
            (err == 0 || vec == 21)) {
            u8 insn[4];
            int skip4 = 0;
            if (guest_fetch(rip, insn, 4) == 0) {
                if (insn[0] == 0xF3 && insn[1] == 0x0F && insn[2] == 0x1E &&
                    (insn[3] == 0xFA || insn[3] == 0xFB))
                    skip4 = 1;
                if (insn[0] == 0x90 && insn[1] == 0x90 &&
                    insn[2] == 0x90 && insn[3] == 0x90)
                    skip4 = 1;
            }
            if (skip4) {
                static u32 endbr_skips;
                if (endbr_skips < 8) {
                    log_hex64("[VMEXIT] skip landing-pad rip=", rip);
                    log_hex64("[VMEXIT] skip landing-pad ilen=", ilen);
                    endbr_skips++;
                }
                vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO, 0);
                vmx_vmcs_write(VMCS_GUEST_RIP, rip + 4);
                *out_resume = 1;
                return 0;
            }
        }
    }
    if (g_linux_guest_active) {
        /* Linux's decompressor intentionally faults while extending its
         * identity map.  Reinject the intercepted hardware exception with
         * its original error code so the guest IDT can service it. */
        if (intr_info & (1ULL << 11)) {
            vmx_vmcs_write(VMCS_VM_ENTRY_EXCEPTION_ERROR,
                           vmx_vmcs_read(VMCS_EXIT_INTR_ERROR_CODE));
        }
        vmx_vmcs_write(VMCS_VM_ENTRY_INTERRUPT_INFO, intr_info);
        *out_resume = 1;
        return 0;
    }
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

    if (g_linux_guest_active)
        linux_cet_restore_host();

    g_vmexit_count++;
    INSTR_STAT_INC(vmexit_total);

    if (g_linux_guest_active &&
        (g_vmexit_count <= 32 || (g_vmexit_count & 0xFFF) == 0)) {
        log_hex64("[VMEXIT] n=", g_vmexit_count);
        log_hex64("[VMEXIT] reason=", reason);
        log_hex64("[VMEXIT] rip=", rip);
        log_hex64("[VMEXIT] qual=", qualification);
        log_hex64("[VMEXIT] ilen=", instr_len);
        log_hex64("[VMEXIT] idtr=", vmx_vmcs_read(VMCS_GUEST_IDTR_BASE));
        if (reason == EXIT_EPT_VIOLATION) {
            log_hex64("[VMEXIT] ept gpa=",
                      vmx_vmcs_read(VMCS_GUEST_PHYSICAL_ADDR));
        }
    }

    switch (reason) {
    case EXIT_HLT:
        handle_hlt(rip, instr_len, &resume);
        break;
    case EXIT_ENTRY_FAIL_MSR_LOADING:
        log_warn("[VMEXIT] VM-entry MSR-load failed; dropping CET MSR list");
        vmx_vmcs_write(VMCS_VM_ENTRY_MSR_LOAD_COUNT, 0);
        resume = 1;
        break;
    case EXIT_EPT_VIOLATION:
        INSTR_STAT_INC(ept_violation);
        handle_ept_violation(qualification, rip, &resume);
        break;
    case EXIT_EPT_MISCONFIG: {
        /* Nested KVM: guest PAT (e.g. WC for virtio-gpu shmem) combined
         * with EPT WB is a reserved effective type → misconfig. Rewrite
         * the leaf with Ignore PAT and resume; do not kill the vCPU. */
        u64 gpa = vmx_vmcs_read(VMCS_GUEST_PHYSICAL_ADDR);
        u64 page = gpa & ~(EPT_PAGE_SIZE - 1);
        static u32 misconfig_logs;
        INSTR_PROBE(EPTM, gpa, rip, 0, 0);
        if (misconfig_logs < 16) {
            misconfig_logs++;
            log_error("[VMEXIT] EPT misconfig");
            log_hex64("[VMEXIT] gpa=", gpa);
            ept_log_walk(page);
        }
        if (IS_VIRTIO_MMIO_GPA(gpa)) {
            resume = 0;
            break;
        }
        if (ept_repair_leaf(page, EPT_RWX) != 0) {
            if (ept_map_guest_ram_page(page) != 0) {
                log_error("[VMEXIT] EPT misconfig repair failed");
                resume = 0;
                break;
            }
            if (misconfig_logs <= 16)
                log_hex64("[VMEXIT] EPT misconfig replaced gpa=", page);
        }
        ept_flush_ept();
        resume = 1;
        break;
    }
    case EXIT_EXCEPTION_NMI:
        INSTR_PROBE(EXCP, qualification, rip, 0, 0);
        handle_exception(qualification, rip, &resume);
        break;
    case EXIT_TRIPLE_FAULT:
        INSTR_PROBE(EXCP, 0, rip, 0, 0);
        log_error("[VMEXIT] triple fault");
        log_hex64("[VMEXIT] triple rip=", rip);
        log_hex64("[VMEXIT] triple cr0=", vmx_vmcs_read(VMCS_GUEST_CR0));
        log_hex64("[VMEXIT] triple cr3=", vmx_vmcs_read(VMCS_GUEST_CR3));
        log_hex64("[VMEXIT] triple cr4=", vmx_vmcs_read(VMCS_GUEST_CR4));
        log_hex64("[VMEXIT] triple idtr=", vmx_vmcs_read(VMCS_GUEST_IDTR_BASE));
        log_hex64("[VMEXIT] triple idtl=", vmx_vmcs_read(VMCS_GUEST_IDTR_LIMIT));
        resume = 0;
        break;
    case EXIT_EXTERNAL_INTERRUPT:
        INSTR_STAT_INC(ext_irq);
        /* 外部中断到达 host（真机比 QEMU 更频繁触发）。
         * 当前 UTSM 不处理 host 外部中断（依赖 PIC/IDT），
         * 直接 resume guest。真机上此 exit 由 PIN_EXT_INTERRUPT_EXITING
         * 触发，不应终止 guest。 */
        resume = 1;
        break;
    case EXIT_INIT_SIGNAL:
        /* INIT 信号在 VMX root 下被阻断，但 non-root 会 exit。
         * 真机 BSP 向 AP 发 INIT-IPI-SIPI 时可能触发。
         * 吞掉：不执行 INIT，直接 resume。 */
        resume = 1;
        break;
    case EXIT_INVD:
        /* INVD（cache flush）：吞掉，直接 resume。 */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_INVLPG:
        /* Guest TLB invalidate. EPT owns the real translation; skip. */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_CONTROL_ACCESS:
        handle_cr_access(qualification, rip, instr_len, &resume);
        break;
    case EXIT_MONITOR:
    case EXIT_MWAIT:
        /* MONITOR/MWAIT：在 noapic 模式下 Linux 不会主动使用，
         * 但某些驱动路径可能触发。直接 resume。 */
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    case EXIT_CPUID:
        INSTR_STAT_INC(cpuid_exit);
        /* host passthrough + 屏蔽 VMX/hypervisor 位（handle_cpuid 内推进 RIP） */
        handle_cpuid(rip, instr_len, &resume);
        break;
    case EXIT_XSETBV:
        /* XSETBV always VM-exits. Linux fpu__init writes XCR0 here. */
        {
            u32 xcr = (u32)g_guest_regs.rcx;
            u64 val = ((u64)(u32)g_guest_regs.rdx << 32) |
                      (u32)g_guest_regs.rax;
            if (xcr == 0)
                linux_xsave_set_guest_xcr0(val);
            vmx_vmcs_write(VMCS_GUEST_RIP, rip + (instr_len ? instr_len : 3));
            resume = 1;
        }
        break;
    case EXIT_WBINVD:
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + (instr_len ? instr_len : 2));
        resume = 1;
        break;
    case EXIT_IO_INSTRUCTION:
        INSTR_STAT_INC(io_exit);
        /* legacy 设备模拟：COM1 UART / PIC / PIT / 其余端口吞掉 */
        handle_io(qualification, rip, instr_len, &resume);
        break;
    case EXIT_INTERRUPT_WINDOW:
        INSTR_STAT_INC(intr_window);
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
        INSTR_STAT_INC(preempt_timer);
        /* 10ms 周期 exit：resume 前由 vmexit_before_resume 统一
         * 轮询 host 串口 RX / 推进 PIT tick / 注入 pending IRQ。
         * Also force IRQ0 here. TSC-based PIT accounting can miss ticks
         * after park/resume, and nanosleep/Xorg then freeze on noapic. */
        vmx_guest_queue_irq(g_pic_vec_base ? g_pic_vec_base : 0x30);
        resume = 1;
        if (g_linux_guest_active && g_linux_timeslice_armed) {
            g_linux_slice_ticks++;
            if (g_linux_slice_ticks >= LINUX_HOST_SLICE_TICKS) {
                g_linux_slice_ticks = 0;
                g_guest_parked = 1;
                resume = 0;
            }
        }
        break;
    case EXIT_RDMSR:
    case EXIT_WRMSR: {
        INSTR_STAT_INC(msr_exit);
        /* 真机 MSR 访问比 QEMU 严格得多。Linux guest 会访问许多 MSR：
         *   - IA32_MISC_ENABLE (0x1A0): QEMU 忽略，真机需要返回合理值
         *   - IA32_APIC_BASE (0x1B): APIC base 地址
         *   - IA32_TSC_AUX (0xC0000103): RDTSCP auxiliary
         *   - IA32_FS_BASE/GS_BASE (0xC0000100/1): 已由 VMCS 管理
         *   - IA32_KERNEL_GS_BASE (0xC0000102): swapgs 目标
         *   - IA32_BNDCFGS (0xD90): MPX bound config（某些 Intel 有）
         * 当前策略：直接 passthrough host MSR 值（读），或忽略写。
         * 注意：WRMSR 写入某些 MSR 可能影响 host，需要白名单保护。
         */
        u64 msr = g_guest_regs.rcx & 0xFFFFFFFFULL;
        if (msr == 0xC0000080ULL) { /* IA32_EFER */
            if (reason == EXIT_RDMSR) {
                u64 val = vmx_vmcs_read(VMCS_GUEST_IA32_EFER);
                g_guest_regs.rax = val & 0xFFFFFFFFULL;
                g_guest_regs.rdx = (val >> 32) & 0xFFFFFFFFULL;
            } else {
                u64 val = (g_guest_regs.rax & 0xFFFFFFFFULL) |
                          (g_guest_regs.rdx << 32);
                val |= EFER_LME | EFER_LMA;
                val &= ~EFER_SVME;
                vmx_vmcs_write(VMCS_GUEST_IA32_EFER, val);
            }
            vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
            resume = 1;
            break;
        }
        /* FS/GS bases live in the VMCS. Ignoring WRMSR leaves TLS at 0. */
        if (msr == 0xC0000100ULL || msr == 0xC0000101ULL || msr == 0xC0000102ULL) {
            static u64 guest_kernel_gs;
            if (reason == EXIT_RDMSR) {
                u64 val = 0;
                if (msr == 0xC0000100ULL) {
                    val = vmx_vmcs_read(VMCS_GUEST_FS_BASE);
                } else if (msr == 0xC0000101ULL) {
                    val = vmx_vmcs_read(VMCS_GUEST_GS_BASE);
                } else {
                    val = guest_kernel_gs;
                }
                g_guest_regs.rax = val & 0xFFFFFFFFULL;
                g_guest_regs.rdx = (val >> 32) & 0xFFFFFFFFULL;
            } else {
                u64 val = (g_guest_regs.rax & 0xFFFFFFFFULL) |
                          (g_guest_regs.rdx << 32);
                if (msr == 0xC0000100ULL) {
                    vmx_vmcs_write(VMCS_GUEST_FS_BASE, val);
                } else if (msr == 0xC0000101ULL) {
                    vmx_vmcs_write(VMCS_GUEST_GS_BASE, val);
                } else {
                    u32 lo = (u32)val;
                    u32 hi = (u32)(val >> 32);
                    guest_kernel_gs = val;
                    /* SWAPGS reads the hardware KERNEL_GS_BASE MSR. */
                    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000102u));
                }
            }
            vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
            resume = 1;
            break;
        }
        if (reason == EXIT_RDMSR) {
            u64 val = 0;
            /* 白名单：允许读取的 MSR（passthrough host 值） */
            int allowed = 0;
            /* 通用允许列表 */
            if (msr == 0x1A0 ||   /* IA32_MISC_ENABLE */
                msr == 0x1B   ||  /* IA32_APIC_BASE */
                msr == 0xC0000100 || /* IA32_FS_BASE */
                msr == 0xC0000101 || /* IA32_GS_BASE */
                msr == 0xC0000102 || /* IA32_KERNEL_GS_BASE */
                msr == 0xC0000103 || /* IA32_TSC_AUX */
                msr == 0xD90  ||    /* IA32_BNDCFGS */
                msr == 0x3A  ||     /* IA32_FEATURE_CONTROL */
                msr == 0x8B  ||     /* IA32_BIOS_SIGN (microcode) */
                msr == 0x17  ||     /* IA32_PLATFORM_ID */
                msr == 0x4B  ||     /* IA32_UCODE_REV (some CPUs) */
                msr == 0xCE  ||     /* IA32_PLATFORM_INFO */
                msr == 0x198 ||     /* IA32_PERF_STATUS */
                msr == 0x199 ||     /* IA32_PERF_CTL */
                msr == 0x1AD ||     /* IA32_THREAD_FEEDBACK_CHAR (Raptor Lake) */
                msr == 0x1AE ||     /* IA32_CORE_THREAD_COUNT */
                (msr >= 0x0 && msr <= 0x1F) || /* Basic CPUID MSRs */
                (msr >= 0x30 && msr <= 0x3F) || /* PMC MSRs */
                (msr >= 0x186 && msr <= 0x18F) || /* Perf MSRs */
                (msr >= 0xC0000000 && msr <= 0xC0000103) /* SYSENTER/EFER etc */
            ) {
                allowed = 1;
            }
            if (allowed) {
                /* 安全读取：用 rdmsr 可能 #GP，用 try-catch 模式 */
                __asm__ volatile(
                    "1: rdmsr\n"
                    "   jmp 2f\n"
                    ".section .text\n"
                    "2:\n"
                    : "=a"(((u32*)&val)[0]), "=d"(((u32*)&val)[1])
                    : "c"(msr)
                );
            }
            g_guest_regs.rax = val & 0xFFFFFFFFULL;
            g_guest_regs.rdx = (val >> 32) & 0xFFFFFFFFULL;
        } else {
            /* WRMSR：白名单保护，防止 guest 修改 host MSR */
            int allowed = 0;
            if (msr == 0xC0000100 || /* IA32_FS_BASE — handled above */
                msr == 0xC0000101 ||
                msr == 0xC0000102 ||
                msr == 0xC0000103 || /* IA32_TSC_AUX */
                msr == 0x1B   ||     /* IA32_APIC_BASE — 忽略（guest noapic） */
                msr == 0xD90  ||     /* IA32_BNDCFGS — 忽略 */
                msr == 0x199  ||     /* IA32_PERF_CTL — 忽略 */
                msr == 0x6A0  ||     /* IA32_U_CET */
                msr == 0x6A2  ||     /* IA32_S_CET */
                msr == 0x6A4  || msr == 0x6A5 || msr == 0x6A6 ||
                msr == 0x6A7  || msr == 0x6A8
            ) {
                allowed = 1;
            }
            /* 不允许的写直接吞掉 */
            if (!allowed) {
                log_hex64("[VMEXIT] WRMSR blocked msr=", msr);
            }
        }
        vmx_vmcs_write(VMCS_GUEST_RIP, rip + instr_len);
        resume = 1;
        break;
    }
    case EXIT_VMCALL:
        INSTR_PROBE(HCALL, g_guest_regs.rax, g_guest_regs.rdi, g_guest_regs.rsi, rip);
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

    /* resume 前例行工作：仅在继续跑 guest 时注入 IRQ / 轮询设备。
     * 不要在 HLT/timeslice 归还 host 时调用——那会在 host 栈上走 guest
     * virtio poll，已触发 CR2=3 的 host #PF。下次 linux_resume 入口再准备。 */
    if (resume) {
        INSTR_STAT_INC(pre_resume);
        vmexit_before_resume();
    }
    return resume;
}

u64 vmexit_get_count(void) { return g_vmexit_count; }
