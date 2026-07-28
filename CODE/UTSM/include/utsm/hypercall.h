#ifndef UTSM_HYPERCALL_H
#define UTSM_HYPERCALL_H

/* UTSM-side VMCALL hypercall handler.
 *
 * When the Linux guest executes a VMCALL instruction, it triggers a VM-Exit
 * with exit reason EXIT_VMCALL (18). The guest places:
 *   RAX = UTSM_HCALL_MAGIC | op    (identifies this as a UTSM hypercall)
 *   RDI = arg0
 *   RSI = arg1
 *   RDX = arg2
 *
 * The handler validates the magic, dispatches to the operation handler,
 * writes the return value into GUEST_RAX, and advances GUEST_RIP past
 * the VMCALL instruction so the guest resumes normally.
 *
 * If the magic doesn't match, the handler returns a VMFAIL indication
 * (by setting the VMCS vm-exit instruction error), which causes the guest
 * to see VMCALL fail. Phase 1.3: we just return -UTSM_HCALL_NOSYS.
 *
 * Protocol details in CODE/utsm-ipc/ipc_proto.h.
 */

#include <utsm/types.h>

/* VM-Exit handler for VMCALL (exit reason 18).
 *
 * Reads guest RAX/RDI/RSI/RDX from the VMCS save area (passed in by the
 * assembly exit stub), dispatches the hypercall, writes the result to
 * GUEST_RAX, and advances GUEST_RIP.
 *
 * Returns 1 to resume the guest, 0 to terminate. */
int hypercall_handle(u64 guest_rax, u64 guest_rdi, u64 guest_rsi, u64 guest_rdx,
                     u64 guest_rip, u64 instr_len);

/* Statistics */
u64 hypercall_get_count(void);

#endif /* UTSM_HYPERCALL_H */
