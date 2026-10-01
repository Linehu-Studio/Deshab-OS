/* LD_PRELOAD into qemu-system-x86_64.
 *
 * Nested KVM does not virtualize CET VMCS fields. L0 U_CET (IBT+SHSTK)
 * leaks into L2: glibc #GPs on landing pads, musl PUSH writes SSP=0.
 *
 * arch_prctl(SHSTK_DISABLE) is EINVAL on WSL (no shstk in cpuinfo).
 * Host-wide wrmsr IA32_U_CET / setting CR4.CET kills UTSM — do not.
 *
 * This library asks *KVM* to load CET MSRs as 0 on the vCPU (L0
 * virtualization, not a host WRMSR). It also keeps the arch_prctl
 * attempt for kernels that do support SHSTK.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif
#ifndef ARCH_SHSTK_DISABLE
#define ARCH_SHSTK_DISABLE 0x5002
#endif
#ifndef ARCH_SHSTK_SHSTK
#define ARCH_SHSTK_SHSTK (1UL << 0)
#endif
#ifndef ARCH_SHSTK_IBT
#define ARCH_SHSTK_IBT (1UL << 1)
#endif

#ifndef KVMIO
#define KVMIO 0xAE
#endif

/* linux/kvm.h: _IO(KVMIO, 0x41/0x80), _IOWR/_IOW of sizeof(kvm_msrs)=8 */
#ifndef KVM_CREATE_VCPU
#define KVM_CREATE_VCPU 0xae41u
#endif
#ifndef KVM_RUN
#define KVM_RUN 0xae80u
#endif
#ifndef KVM_GET_MSRS
#define KVM_GET_MSRS 0xc008ae88u
#endif
#ifndef KVM_SET_MSRS
#define KVM_SET_MSRS 0x4008ae89u
#endif

#define MSR_IA32_U_CET              0x000006a0u
#define MSR_IA32_S_CET              0x000006a2u
#define MSR_IA32_PL0_SSP            0x000006a4u
#define MSR_IA32_PL1_SSP            0x000006a5u
#define MSR_IA32_PL2_SSP            0x000006a6u
#define MSR_IA32_PL3_SSP            0x000006a7u
#define MSR_IA32_INTERRUPT_SSP_TABLE 0x000006a8u

struct kvm_msr_entry {
    uint32_t index;
    uint32_t reserved;
    uint64_t data;
};

struct kvm_msrs {
    uint32_t nmsrs;
    uint32_t pad;
    struct kvm_msr_entry entries[];
};

static int (*real_ioctl)(int, unsigned long, ...);
static int g_log_fd = -1;

static void cet_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    if (g_log_fd < 0) {
        g_log_fd = open("/tmp/deshab_cet.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (g_log_fd < 0)
            return;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        (void)write(g_log_fd, buf, (size_t)n);
}

static void resolve_ioctl(void)
{
    if (!real_ioctl) {
        real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
            real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_DEFAULT, "ioctl");
    }
}

static int kvm_ioctl(int fd, unsigned long req, void *arg)
{
    resolve_ioctl();
    if (!real_ioctl)
        return syscall(SYS_ioctl, fd, req, arg);
    return real_ioctl(fd, req, arg);
}

static void try_vcpu_cet_zero(int fd)
{
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry e[7];
    } buf;
    static int logged_get;
    int r, saved;
    unsigned i;

    memset(&buf, 0, sizeof(buf));
    buf.hdr.nmsrs = 7;
    buf.e[0].index = MSR_IA32_U_CET;
    buf.e[1].index = MSR_IA32_S_CET;
    buf.e[2].index = MSR_IA32_PL0_SSP;
    buf.e[3].index = MSR_IA32_PL1_SSP;
    buf.e[4].index = MSR_IA32_PL2_SSP;
    buf.e[5].index = MSR_IA32_PL3_SSP;
    buf.e[6].index = MSR_IA32_INTERRUPT_SSP_TABLE;

    saved = errno;
    r = kvm_ioctl(fd, KVM_GET_MSRS, &buf);
    if (!logged_get) {
        logged_get = 1;
        cet_log("KVM_GET_MSRS CET fd=%d r=%d errno=%d", fd, r, r < 0 ? errno : 0);
        for (i = 0; i < 7; i++)
            cet_log("  [%u] msr=0x%x data=0x%llx",
                    i, buf.e[i].index, (unsigned long long)buf.e[i].data);
        cet_log("\n");
    }
    errno = saved;

    memset(&buf, 0, sizeof(buf));
    buf.hdr.nmsrs = 7;
    buf.e[0].index = MSR_IA32_U_CET;
    buf.e[1].index = MSR_IA32_S_CET;
    buf.e[2].index = MSR_IA32_PL0_SSP;
    buf.e[3].index = MSR_IA32_PL1_SSP;
    buf.e[4].index = MSR_IA32_PL2_SSP;
    buf.e[5].index = MSR_IA32_PL3_SSP;
    buf.e[6].index = MSR_IA32_INTERRUPT_SSP_TABLE;
    /* data already 0 */

    saved = errno;
    r = kvm_ioctl(fd, KVM_SET_MSRS, &buf);
    cet_log("KVM_SET_MSRS CET=0 fd=%d r=%d errno=%d\n", fd, r, r < 0 ? errno : 0);
    errno = saved;
}

__attribute__((constructor)) static void deshab_qemu_disable_cet(void)
{
    int rc1, rc2;
    rc1 = (int)syscall(SYS_arch_prctl, ARCH_SHSTK_DISABLE, ARCH_SHSTK_SHSTK);
    rc2 = (int)syscall(SYS_arch_prctl, ARCH_SHSTK_DISABLE, ARCH_SHSTK_IBT);
    cet_log("arch_prctl SHSTK=%d IBT=%d errno_last=%d\n", rc1, rc2, errno);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg = NULL;
    int r;
    static unsigned char did_cet[1024];

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    resolve_ioctl();
    if (!real_ioctl)
        return syscall(SYS_ioctl, fd, request, arg);

    /* Zero CET on the vCPU *before* the first KVM_RUN so L2 does not
     * inherit L0 U_CET. CREATE_VCPU returns the vCPU fd afterwards. */
    if (request == KVM_RUN && fd >= 0 && fd < (int)sizeof(did_cet) && !did_cet[fd]) {
        did_cet[fd] = 1;
        try_vcpu_cet_zero(fd);
    }

    r = real_ioctl(fd, request, arg);

    if (request == KVM_CREATE_VCPU && r >= 0) {
        try_vcpu_cet_zero(r);
        if (r < (int)sizeof(did_cet))
            did_cet[r] = 1;
    }
    return r;
}
