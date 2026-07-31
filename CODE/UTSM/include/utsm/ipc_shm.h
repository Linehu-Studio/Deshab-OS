#ifndef UTSM_IPC_SHM_H
#define UTSM_IPC_SHM_H

/* UTSM-side IPC shared memory manager.
 *
 * Allocates a 1MB contiguous physical region, initializes the
 * utsm_ipc_shm structure (header + two ring buffers + payload pool),
 * and EPT-maps it at LINUX_GUEST_IPC_SHM_GPA so the Linux guest can
 * access it after querying the GPA via UTSM_HCALL_SHM_INFO.
 *
 * The shared protocol structures are defined in CODE/utsm-ipc/ipc_proto.h,
 * which is included by both UTSM and the Linux utsm-ipc driver.
 *
 * Ring buffer discipline:
 *   utsm_to_linux: UTSM produces (ipc_shm_send), Linux consumes
 *   linux_to_utsm: Linux produces, UTSM consumes (ipc_shm_recv)
 */

#include <utsm/types.h>
#include <ipc_proto.h>   /* shared UTSM↔Linux IPC protocol (struct utsm_ipc_msg, etc.) */

/* ===== Initialization ===== */

/* Allocate, initialize, and EPT-map the IPC shared memory region.
 * Must be called after vmm_init() (EPT is required) and before linux_launch().
 * Returns 0 on success, negative on error. */
int ipc_shm_init(void);

/* Shut down IPC: mark UTSM as not ready.
 * Does not free the shared memory (it may still be referenced by EPT). */
void ipc_shm_shutdown(void);

/* Query state */
int ipc_shm_is_ready(void);

/* Get the HPA of the shared memory region (for EPT mapping).
 * Returns 0 if not initialized. */
u64 ipc_shm_get_hpa(void);

/* Get the GPA of the shared memory region (passed to Linux via hypercall). */
u64 ipc_shm_get_gpa(void);

/* Get the size of the shared memory region. */
u64 ipc_shm_get_size(void);

/* ===== Payload pool access (bulk data channel) =====
 *
 * The payload_pool area of the SHM region carries bulk file-transfer data
 * (Phase 3 FILE_LIST/FILE_READ). Linux daemon writes into it (via
 * UTSM_IOCTL_WRITE_POOL) before sending UTSM_MSG_FILE_RESPONSE; UTSM reads
 * it directly through its own HHDM mapping. */

/* Host virtual pointer into payload_pool at byte offset.
 * Returns NULL if not initialized or offset out of range. */
void *ipc_shm_payload_ptr(u32 offset);

/* Total capacity of the payload pool in bytes. */
u32 ipc_shm_payload_capacity(void);

/* ===== Message API (UTSM-side) ===== */

/* Send a message from UTSM to Linux (enqueues into utsm_to_linux ring).
 * Returns 0 on success, -1 if the ring is full. */
int ipc_shm_send(u32 msg_type, const void *data, u32 data_len);

/* Receive a message from Linux to UTSM (dequeues from linux_to_utsm ring).
 * Returns 0 on success (msg filled in), -1 if the ring is empty. */
int ipc_shm_recv(struct utsm_ipc_msg *msg_out);

/* Check if there are pending messages from Linux. */
int ipc_shm_has_pending(void);

/* Notify Linux that a message is waiting (placeholder for IPI injection).
 * Phase 1.3: logs the event; Phase 1.4+ will inject a guest IRQ. */
void ipc_shm_notify_linux(void);

/* ===== Debug ===== */

/* Dump ring buffer statistics to the UTSM serial console. */
void ipc_shm_dump_stats(void);

#endif /* UTSM_IPC_SHM_H */
