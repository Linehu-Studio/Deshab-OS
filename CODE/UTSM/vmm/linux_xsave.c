/* linux_xsave.c — L2 guest XSAVE save/restore.
 *
 * Host UTSM is compiled -mno-sse. A naive XRSTOR of guest AVX/SSE
 * state #GPs. Advertise XSAVE to the Linux guest only after we can
 * XSAVE the guest area before returning to DSK, and XRSTOR it again
 * on the next vmresume.
 *
 * RFBM/XCR0 = 7 (x87+SSE+AVX/YMM). persist glibc is x86-64-v3. */

#include <utsm/linux_xsave.h>
#include <utsm/vmx.h>
#include <utsm/log.h>
#include <utsm/types.h>

#define LINUX_XSAVE_XCR0   0x7ULL
#define LINUX_XSAVE_BYTES  832u
#define LINUX_XSAVE_YMM_SIZE    256u
#define LINUX_XSAVE_YMM_OFF     576u

static int g_xsave_ok;
static u64 g_guest_xcr0 = LINUX_XSAVE_XCR0;
static u64 g_host_xcr0 = LINUX_XSAVE_XCR0;
static u8 g_guest_xsave[4096] __attribute__((aligned(64)));
static u8 g_host_xsave[4096] __attribute__((aligned(64)));

static void xsave_zero(u8 *p, usize n) {
    for (usize i = 0; i < n; i++)
        p[i] = 0;
}

static void xsave_enable_osxsave(void) {
    u64 cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(cr4 & CR4_OSXSAVE)) {
        cr4 |= CR4_OSXSAVE;
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
    /* VM-exit reloads HOST_CR4. Keep OSXSAVE so later XSAVE is legal. */
    vmx_vmcs_write(VMCS_HOST_CR4, cr4 | CR4_VMXE);
}

static void xsave_xsetbv(u64 xcr0) {
    u32 lo = (u32)xcr0;
    u32 hi = (u32)(xcr0 >> 32);
    __asm__ volatile(".byte 0x0f, 0x01, 0xd1"
                     :
                     : "a"(lo), "c"(0), "d"(hi)
                     : "memory");
}

static void xsave_mem(void *ptr, u64 rfbm) {
    u32 lo = (u32)rfbm;
    u32 hi = (u32)(rfbm >> 32);
    __asm__ volatile(".byte 0x0f, 0xae, 0x27" /* xsave (%rdi) */
                     :
                     : "D"(ptr), "a"(lo), "d"(hi)
                     : "memory");
}

static void xrstor_mem(void *ptr, u64 rfbm) {
    u32 lo = (u32)rfbm;
    u32 hi = (u32)(rfbm >> 32);
    __asm__ volatile(".byte 0x0f, 0xae, 0x2f" /* xrstor (%rdi) */
                     :
                     : "D"(ptr), "a"(lo), "d"(hi)
                     : "memory");
}

int linux_xsave_available(void) {
    return g_xsave_ok;
}

void linux_xsave_init(void) {
    u32 a, b, c, d;

    if (g_xsave_ok)
        return;

    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    if (!(c & (1u << 26))) {
        log_warn("[LINUX] XSAVE not in host CPUID; persist glibc stays hidden");
        return;
    }

    xsave_zero(g_guest_xsave, sizeof(g_guest_xsave));
    xsave_zero(g_host_xsave, sizeof(g_host_xsave));
    g_guest_xcr0 = LINUX_XSAVE_XCR0;
    g_host_xcr0 = LINUX_XSAVE_XCR0;

    xsave_enable_osxsave();
    xsave_xsetbv(LINUX_XSAVE_XCR0);
    /* Init-state XRSTOR so the first vmlaunch does not inherit host junk. */
    xrstor_mem(g_guest_xsave, LINUX_XSAVE_XCR0);

    g_xsave_ok = 1;
    log_info("[LINUX] XSAVE ready for L2 (x87+SSE+AVX, XCR0=7)");
    log_hex64("[LINUX] XSAVE area bytes=", LINUX_XSAVE_BYTES);
}

void linux_xsave_save_guest(void) {
    if (!g_xsave_ok)
        return;
    xsave_enable_osxsave();
    xsave_xsetbv(g_guest_xcr0);
    xsave_mem(g_guest_xsave, g_guest_xcr0);
}

void linux_xsave_load_guest(void) {
    if (!g_xsave_ok)
        return;
    xsave_enable_osxsave();
    xsave_xsetbv(g_guest_xcr0);
    xrstor_mem(g_guest_xsave, g_guest_xcr0);
}

void linux_xsave_save_host(void) {
    if (!g_xsave_ok)
        return;
    xsave_enable_osxsave();
    xsave_xsetbv(g_host_xcr0);
    xsave_mem(g_host_xsave, g_host_xcr0);
}

void linux_xsave_load_host(void) {
    if (!g_xsave_ok)
        return;
    xsave_enable_osxsave();
    xsave_xsetbv(g_host_xcr0);
    xrstor_mem(g_host_xsave, g_host_xcr0);
}

void linux_xsave_set_guest_xcr0(u64 xcr0) {
    u64 allowed = LINUX_XSAVE_XCR0;
    u64 v = xcr0 & allowed;
    static int logged;

    if (!g_xsave_ok)
        return;
    /* Intel requires x87. Keep SSE if this build advertises it. */
    if (!(v & 1ULL))
        v |= 1ULL;
    if ((allowed & 2ULL) && !(v & 2ULL))
        v |= 2ULL;
    if (!logged && v != xcr0) {
        logged = 1;
        log_hex64("[LINUX] XSETBV clamped from=", xcr0);
        log_hex64("[LINUX] XSETBV clamped to=", v);
    }
    g_guest_xcr0 = v;
    xsave_enable_osxsave();
    xsave_xsetbv(v);
}

void linux_xsave_adjust_cpuid(u32 leaf, u32 subleaf,
                              u32 *eax, u32 *ebx, u32 *ecx, u32 *edx,
                              u64 guest_cr4) {
    if (!eax || !ebx || !ecx || !edx)
        return;

    if (!g_xsave_ok) {
        if (leaf == 1) {
            *ecx &= ~((1u << 26) | (1u << 27) | (1u << 28));
        }
        if (leaf == 0xD) {
            *eax = 0;
            *ebx = 0;
            *ecx = 0;
            *edx = 0;
        }
        return;
    }

    if (leaf == 1) {
        *ecx |= (1u << 26); /* XSAVE */
        if (guest_cr4 & CR4_OSXSAVE)
            *ecx |= (1u << 27);
        else
            *ecx &= ~(1u << 27);
        *ecx |= (1u << 28); /* AVX / XCR0 bit 2 */
        return;
    }

    if (leaf != 0xD)
        return;

    /* Hide XSAVEOPT/XSAVEC/XSAVES. Standard XSAVE only. */
    if (subleaf == 0) {
        *eax = (u32)LINUX_XSAVE_XCR0;
        *ebx = LINUX_XSAVE_BYTES;
        *ecx = LINUX_XSAVE_BYTES;
        *edx = 0;
    } else if (subleaf == 2) {
        *eax = LINUX_XSAVE_YMM_SIZE;
        *ebx = LINUX_XSAVE_YMM_OFF;
        *ecx = 0;
        *edx = 0;
    } else {
        *eax = 0;
        *ebx = 0;
        *ecx = 0;
        *edx = 0;
    }
}
