#ifndef UTSM_LINUX_XSAVE_H
#define UTSM_LINUX_XSAVE_H

#include <utsm/types.h>

/* L2 Linux XSAVE (x87+SSE first). UTSM is built -mno-sse, so every
 * XSAVE/XRSTOR/XSETBV goes through raw opcodes. Guest state must be
 * saved before returning to DSK/desktop and restored on linux_resume. */

int linux_xsave_available(void);
void linux_xsave_init(void);
void linux_xsave_save_guest(void);
void linux_xsave_load_guest(void);
void linux_xsave_save_host(void);
void linux_xsave_load_host(void);
void linux_xsave_set_guest_xcr0(u64 xcr0);
void linux_xsave_adjust_cpuid(u32 leaf, u32 subleaf,
                              u32 *eax, u32 *ebx, u32 *ecx, u32 *edx,
                              u64 guest_cr4);

#endif
