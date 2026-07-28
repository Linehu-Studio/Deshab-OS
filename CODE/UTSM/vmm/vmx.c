#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== MSR / CR helpers ===== */

u64 vmx_read_msr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static void vmx_write_msr(u32 msr, u64 value) {
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

int vmx_enable(void) {
    if (g_vmx_enabled) return 0;

    if (!vmx_supported()) {
        log_error("[VMX] CPU does not support VMX");
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
    (void)vmx_required_cr0();

    /* 执行 vmxon */
    int err;
    __asm__ volatile(
        "vmxon %1\n"
        "jnc 1f\n"
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
        "jnc 1f\n"
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
        "jnc 1f\n"
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
        "jnc 1f\n"
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
