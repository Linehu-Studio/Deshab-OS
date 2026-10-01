#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;
extern volatile struct limine_kernel_address_request g_kernel_address_request;

/* ===== MSR / CR helpers ===== */

u64 vmx_read_msr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

u64 vmx_adjust_control(u64 value, u32 msr) {
    u64 raw = vmx_read_msr(msr);
    u32 allowed0 = (u32)raw;
    u32 allowed1 = (u32)(raw >> 32);
    /* KVM / nested-KVM polarity (verified 2026-09-06):
     *   low bit=1  => must be 1
     *   high bit=0 => must be 0
     * Secondary CTLS2 reports allowed0=0; inverting low would force every
     * allowed1 feature on and fail VM-entry check 7. */
    u32 adjusted = ((u32)value | allowed0) & allowed1;
    log_hex64("[VMX] adjust msr=", msr);
    log_hex64("[VMX] adjust desired=", value);
    log_hex64("[VMX] adjust allowed0=", allowed0);
    log_hex64("[VMX] adjust allowed1=", allowed1);
    log_hex64("[VMX] adjust result=", adjusted);
    return adjusted;
}

u64 vmx_kernel_virt_to_phys(const void *address) {
    const struct limine_kernel_address_response *response =
        g_kernel_address_request.response;
    u64 virt = (u64)address;

    if (!response) {
        log_error("[VMX] Limine kernel-address response missing");
        return 0;
    }
    if (virt < response->virtual_base) {
        log_error("[VMX] address is below kernel virtual base");
        return 0;
    }

    return response->physical_base + (virt - response->virtual_base);
}

void vmx_write_msr(u32 msr, u64 value) {
    u32 lo = (u32)(value & 0xFFFFFFFFULL);
    u32 hi = (u32)(value >> 32);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

static inline u64 read_cr0(void) {
    u64 v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline u64 read_cr4(void) {
    u64 v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(u64 v) {
    __asm__ volatile("mov %0, %%cr0" :: "r"(v));
}
static inline void write_cr4(u64 v) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(v));
}

static inline u64 read_cr3(void) {
    u64 v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline u64 read_rflags(void) {
    u64 v;
    __asm__ volatile("pushfq; popq %0" : "=r"(v));
    return v;
}

/* ===== CPUID ===== */

static void cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

int vmx_supported(void) {
    u32 eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1U << 5)) != 0;   /* CPUID.1:ECX[5] = VMX */
}

/* ===== VMXON region ===== */

static u64 g_vmxon_phys;
static u64 g_vmcs_revision;
static u64 g_vmx_basic;
static int g_vmx_enabled;

/* VMX 要求 CR0 固定位必须为 1（VMX operation 不允许清除）。
 * 这些位由 IA32_VMX_BASIC MSR 的高 32 位（CR0 fixed-0）和
 * IA32_VMX_CR0_FIXED1 MSR 决定。简化：直接 OR 上 PE/NE/PG/WP。 */
static u64 vmx_required_cr0(void) {
    return read_cr0() | CR0_PE | CR0_NE | CR0_WP | CR0_PG;
}

static u64 vmx_required_cr4(void) {
    return read_cr4() | CR4_VMXE | CR4_PAE | CR4_PGE;
}

/* Check whether we are running under a hypervisor that does NOT support
 * nested VMX. This covers:
 *   - WHPX: CPUID may report VMX but VMXON/CR4.VMXE causes fatal exit
 *   - Any hypervisor without nested virtualization support
 *
 * Detection strategy:
 *   1. Check CPUID.01H:ECX[31] (hypervisor present bit)
 *   2. If hypervisor present, check CPUID.40000000H for vendor signature
 *   3. Try reading IA32_VMX_BASIC MSR — if it returns 0, VMX is not real
 *   4. Check Hyper-V specific: HV_CPU_MANAGEMENT_FEATURES for nested VMX
 *
 * If VMX CPUID flag is set but we're under a hypervisor that doesn't expose
 * real VMX MSRs, we must not attempt CR4.VMXE or VMXON, as this will cause
 * a fatal VM exit in WHPX (exit code 4).
 */
static int vmx_truly_available(void) {
    /* Step 1: If CPUID doesn't even report VMX, definitely not available */
    if (!vmx_supported()) return 0;

    /* Step 2: Check if running under a hypervisor */
    u32 eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1U << 31))) {
        /* No hypervisor — bare metal, VMX should work */
        return 1;
    }
    log_info("[VMX] hypervisor present bit set, checking nested VMX support");

    /* Step 3: Try reading IA32_VMX_BASIC — if it's zero or faults,
     * the hypervisor doesn't expose real VMX. */
    /* We use a safe read: if the MSR doesn't exist, rdmsr would #GP.
     * However, under WHPX, IA32_VMX_BASIC may return a non-zero value
     * even though VMXON will fail. So we need another check. */

    /* Step 4: Check hypervisor vendor for nested VMX support */
    cpuid(0x40000000, &eax, &ebx, &ecx, &edx);

    /* P8.3 Fix: diagnostic logging of hypervisor vendor */
    {
        char hv[13];
        hv[0] = (char)(ebx & 0xFF); hv[1] = (char)((ebx >> 8) & 0xFF);
        hv[2] = (char)((ebx >> 16) & 0xFF); hv[3] = (char)((ebx >> 24) & 0xFF);
        hv[4] = (char)(ecx & 0xFF); hv[5] = (char)((ecx >> 8) & 0xFF);
        hv[6] = (char)((ecx >> 16) & 0xFF); hv[7] = (char)((ecx >> 24) & 0xFF);
        hv[8] = (char)(edx & 0xFF); hv[9] = (char)((edx >> 8) & 0xFF);
        hv[10] = (char)((edx >> 16) & 0xFF); hv[11] = (char)((edx >> 24) & 0xFF);
        hv[12] = 0;
        log_info("[VMX] hypervisor vendor: ");
        log_info(hv);
    }

    /* Check for Hyper-V ("Microsoft Hv") */
    if (ebx == 0x7263694DU && ecx == 0x666F736FU && edx == 0x76482074U) {
        log_info("[VMX] Hyper-V detected");
        /* Check if HV_CPU_MANAGEMENT_FEATURES (MSR 0x4000000F) exists.
         * Hyper-V CPUID leaf 0x40000003 tells us which MSRs are available.
         * Partition_privileges bit 1 (nested virtualization) is at EBX bit 1
         * of CPUID 0x40000000 + 3 = 0x40000003.
         * But safer: just check the max CPUID leaf for Hyper-V. */
        u32 hv_max_leaf = eax;
        if (hv_max_leaf >= 0x40000003) {
            /* CPUID 0x40000003 EBX bit 1 = AccessIntrCtrl (nested) */
            cpuid(0x40000003, &eax, &ebx, &ecx, &edx);
            if (ebx & (1U << 1)) {
                log_info("[VMX] Hyper-V nested virtualization supported");
                return 1;
            }
        }
        log_warn("[VMX] Hyper-V WITHOUT nested VMX support");
        log_warn("[VMX] CR4.VMXE / VMXON would cause fatal exit, skipping");
        return 0;
    }

    /* P8.3 Fix: Check for KVM ("KVMKVMKVM"). Match ebx+ecx only
     * (edx was wrongly 0x4D564B4D, should be 0x0000004D for "M\0\0\0"). */
    if (ebx == 0x4B4D564BU && ecx == 0x564B4D56U) {
        log_info("[VMX] KVM detected - nested VMX may be available");
        return 1;
    }

    /* P8.3 Fix: Unknown hypervisor - try IA32_VMX_BASIC MSR. */
    {
        u64 vmx_basic = vmx_read_msr(IA32_VMX_BASIC);
        if (vmx_basic != 0) {
            log_info("[VMX] unknown hypervisor but IA32_VMX_BASIC non-zero, attempting");
            return 1;
        }
    }
    log_warn("[VMX] unknown hypervisor, IA32_VMX_BASIC=0, VMX not available");
    return 0;
}

/* P8.5: VMX 指令成功条件是 CF=0 且 ZF=0（即 ja）。
 * 旧代码只查 CF（jnc）：VMfailInvalid（ZF=1, CF=0）会被误判为成功。 */
int vmx_enable(void) {
    if (g_vmx_enabled) return 0;

    if (!vmx_truly_available()) {
        log_warn("[VMX] VMX not truly available, skipping VMX init");
        return -1;
    }

    /* 读 IA32_VMX_BASIC，获取 VMCS revision ID 与 region 大小 */
    g_vmx_basic = vmx_read_msr(IA32_VMX_BASIC);
    g_vmcs_revision = g_vmx_basic & 0x7FFFFFFFULL;
    u32 vmcs_size = (u32)((g_vmx_basic >> 32) & 0x1FFF);
    if (vmcs_size == 0) vmcs_size = 4096;

    log_hex64("[VMX] VMX_BASIC=", g_vmx_basic);
    log_hex64("[VMX] VMCS revision=", g_vmcs_revision);

    /* 分配 VMXON region（4KB 对齐，物理页） */
    dkm_dma_buffer vmxon_buf;
    if (dma_alloc_pages(1, EPT_PAGE_SIZE, 0x100000000ULL, &vmxon_buf) != 0) {
        log_error("[VMX] failed to alloc VMXON region");
        return -2;
    }
    g_vmxon_phys = vmxon_buf.phys;

    /* 在 VMXON region 偏移 0 写入 VMCS revision ID */
    u32 *vmxon_ptr = (u32 *)vmxon_buf.virt;
    vmxon_ptr[0] = (u32)g_vmcs_revision;
    log_hex64("[VMX] VMXON phys=", g_vmxon_phys);

    /* 设置 CR4.VMXE */
    u64 cr4 = vmx_required_cr4();
    write_cr4(cr4);
    log_hex64("[VMX] CR4=", cr4);

    /* 确保 CR0 满足 VMX 要求（这里只读不写，UEFI 已配置好） */
    /* P8.3 Fix: ensure CR0 meets VMX requirements before VMXON */
    u64 cr0 = vmx_required_cr0();
    write_cr0(cr0);
    log_hex64("[VMX] CR0=", cr0);

    /* 执行 vmxon */
    int err;
    __asm__ volatile(
        "vmxon %1\n"
        "ja 1f\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "jmp 3f\n"
        "2:\n"
        "mov $1, %0\n"
        "3:\n"
        : "=r"(err)
        : "m"(g_vmxon_phys)
        : "memory"
    );

    if (err) {
        /* 第二次尝试：读 vmx instruction error 通过 VMCS 不适用 vmxon；
         * 简化：返回失败。 */
        log_error("[VMX] vmxon failed");
        return -3;
    }

    g_vmx_enabled = 1;
    log_info("[VMX] VMX root mode enabled");
    return 0;
}

int vmx_disable(void) {
    if (!g_vmx_enabled) return 0;
    __asm__ volatile("vmxoff" ::: "memory");
    g_vmx_enabled = 0;
    /* 清 CR4.VMXE */
    u64 cr4 = read_cr4();
    cr4 &= ~CR4_VMXE;
    write_cr4(cr4);
    log_info("[VMX] VMX root mode disabled");
    return 0;
}

int vmx_vmcs_alloc(u64 *phys_out) {
    if (!phys_out) return -1;
    dkm_dma_buffer buf;
    if (dma_alloc_pages(1, EPT_PAGE_SIZE, 0x100000000ULL, &buf) != 0) {
        log_error("[VMX] failed to alloc VMCS region");
        return -2;
    }
    /* 偏移 0 写入 VMCS revision ID */
    u32 *p = (u32 *)buf.virt;
    p[0] = (u32)g_vmcs_revision;
    *phys_out = buf.phys;
    return 0;
}

int vmx_vmcs_load(u64 phys) {
    int err;
    __asm__ volatile(
        "vmptrld %1\n"
        "ja 1f\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "jmp 3f\n"
        "2:\n"
        "mov $1, %0\n"
        "3:\n"
        : "=r"(err)
        : "m"(phys)
        : "memory"
    );
    if (err) {
        log_error("[VMX] vmptrld failed");
        return -1;
    }
    return 0;
}

/* P8.4: VMCLEAR - put VMCS in "clear" state (required before VMLAUNCH). */
int vmx_vmcs_clear(u64 phys) {
    int err;
    __asm__ volatile(
        "vmclear %1\n"
        "ja 1f\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "jmp 3f\n"
        "2:\n"
        "mov $1, %0\n"
        "3:\n"
        : "=r"(err)
        : "m"(phys)
        : "memory"
    );
    if (err) {
        log_error("[VMX] vmclear failed");
        return -1;
    }
    return 0;
}

u64 vmx_vmcs_read(u64 field) {
    u64 value;
    __asm__ volatile(
        "vmread %1, %0\n"
        : "=r"(value)
        : "r"(field)
        : "memory"
    );
    return value;
}

int vmx_vmcs_read_checked(u64 field, u64 *value) {
    u64 v;
    u8 ok;
    __asm__ volatile(
        "vmread %2, %1\n"
        "seta %0\n"          /* CF=0 且 ZF=0 才是 VMsucceed */
        : "=q"(ok), "=r"(v)
        : "r"(field)
        : "memory"
    );
    if (ok) {
        if (value) *value = v;
        return 0;
    }
    return -1;
}

void vmx_vmcs_write(u64 field, u64 value) {
    __asm__ volatile(
        "vmwrite %1, %0\n"
        :: "r"(field), "r"(value)
        : "memory"
    );
}

int vmx_vmlaunch(void) {
    int err;
    __asm__ volatile(
        "vmlaunch\n"
        "ja 1f\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "jmp 3f\n"
        "2:\n"
        "mov $1, %0\n"
        "3:\n"
        : "=r"(err)
        :: "memory"
    );
    return err;
}

int vmx_vmresume(void) {
    int err;
    __asm__ volatile(
        "vmresume\n"
        "ja 1f\n"
        "jmp 2f\n"
        "1:\n"
        "mov $0, %0\n"
        "jmp 3f\n"
        "2:\n"
        "mov $1, %0\n"
        "3:\n"
        : "=r"(err)
        :: "memory"
    );
    return err;
}

/* 暴露给其它模块用的辅助 */
u64 vmx_get_host_cr3(void) { return read_cr3(); }
u64 vmx_get_host_rflags(void) { return read_rflags(); }
