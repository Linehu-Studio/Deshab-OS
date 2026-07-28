/* ipc_shm.c — UTSM-side IPC shared memory manager.
 *
 * Allocates a 1MB contiguous physical region, initializes the shared
 * utsm_ipc_shm structure (header + two ring buffers + payload pool),
 * and EPT-maps it at LINUX_GUEST_IPC_SHM_GPA (0x04000000) so the Linux
 * guest can access it after querying the GPA via UTSM_HCALL_SHM_INFO.
 *
 * Ring buffer discipline (single-producer, single-consumer):
 *   utsm_to_linux: UTSM produces via ipc_shm_send, Linux consumes
 *   linux_to_utsm: Linux produces, UTSM consumes via ipc_shm_recv
 *
 * Memory barriers ensure correct visibility across the two kernels
 * (which share the same physical memory via EPT).
 */

#include <utsm/ipc_shm.h>
#include <utsm/vmx.h>
#include <utsm/ept.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/dma.h>
#include <utsm/linux_loader.h>
#include "../arch/x86_64/limine.h"

extern volatile struct limine_hhdm_request g_hhdm_request;

/* ===== Internal state ===== */

static dkm_dma_buffer g_shm_buf;     /* allocated DMA buffer for the SHM region */
static struct utsm_ipc_shm *g_shm;   /* virtual address (HHDM) of the SHM region */
static int g_shm_ready;
static u32 g_send_seq;               /* monotonic sequence for UTSM→Linux messages */
static u64 g_send_count;
static u64 g_recv_count;

/* ===== Initialization ===== */

int ipc_shm_init(void) {
    if (g_shm_ready) {
        log_warn("[IPC] shm already initialized");
        return 0;
    }

    log_info("[IPC] shm init begin");

    /* 1. Allocate 1MB contiguous physical memory for the shared region.
     * Alignment: 4KB (page-aligned). Max phys: none (EPT maps any HPA). */
    u64 page_count = UTSM_IPC_SHM_SIZE / 4096;
    if (dma_alloc_pages(page_count, 4096, 0, &g_shm_buf) != 0) {
        log_error("[IPC] failed to alloc shared memory (1MB)");
        return -1;
    }
    log_hex64("[IPC] shm HPA=", g_shm_buf.phys);
    log_hex64("[IPC] shm virt=", (u64)g_shm_buf.virt);
    log_hex64("[IPC] shm size=", g_shm_buf.size);

    /* 2. Zero the entire region */
    u8 *p = (u8 *)g_shm_buf.virt;
    for (u64 i = 0; i < g_shm_buf.size; i++) {
        p[i] = 0;
    }

    /* 3. Initialize the shared memory structure */
    g_shm = (struct utsm_ipc_shm *)g_shm_buf.virt;

    /* Header */
    g_shm->header.magic = UTSM_IPC_MAGIC;
    g_shm->header.version = UTSM_IPC_VERSION;
    g_shm->header.header_size = sizeof(struct utsm_ipc_shm_header);
    g_shm->header.shm_phys = LINUX_GUEST_IPC_SHM_GPA;  /* GPA as seen by Linux */
    g_shm->header.shm_size = UTSM_IPC_SHM_SIZE;
    g_shm->header.utsm_ready = 1;      /* UTSM is ready now */
    g_shm->header.linux_ready = 0;     /* Linux will set this when it connects */

    /* Ring buffers */
    utsm_ipc_ring_init(&g_shm->utsm_to_linux);
    utsm_ipc_ring_init(&g_shm->linux_to_utsm);

    /* Memory barrier: ensure all writes are visible before EPT mapping */
    utsm_ipc_mb();

    /* 4. EPT-map the shared memory at the guest-visible GPA.
     * Both kernels need read/write access. Execute is not needed. */
    if (ept_map_range(LINUX_GUEST_IPC_SHM_GPA, g_shm_buf.phys,
                      UTSM_IPC_SHM_SIZE, EPT_READ | EPT_WRITE) != 0) {
        log_error("[IPC] EPT map shared memory failed");
        return -2;
    }
    log_hex64("[IPC] shm mapped GPA=", LINUX_GUEST_IPC_SHM_GPA);

    g_shm_ready = 1;
    g_send_seq = 0;
    g_send_count = 0;
    g_recv_count = 0;

    log_info("[IPC] shm init ok");
    return 0;
}

void ipc_shm_shutdown(void) {
    if (!g_shm_ready) return;
    g_shm->header.utsm_ready = 0;
    utsm_ipc_mb();
    g_shm_ready = 0;
    log_info("[IPC] shm shutdown");
}

int ipc_shm_is_ready(void) {
    return g_shm_ready;
}

u64 ipc_shm_get_hpa(void) {
    return g_shm_ready ? g_shm_buf.phys : 0;
}

u64 ipc_shm_get_gpa(void) {
    return LINUX_GUEST_IPC_SHM_GPA;
}

u64 ipc_shm_get_size(void) {
    return UTSM_IPC_SHM_SIZE;
}

/* ===== Message API (UTSM-side) ===== */

int ipc_shm_send(u32 msg_type, const void *data, u32 data_len) {
    if (!g_shm_ready || !g_shm) {
        return -1;
    }

    /* Check if Linux is ready to receive */
    if (!g_shm->header.linux_ready) {
        log_warn("[IPC] send: Linux not ready, dropping message");
        return -1;
    }

    /* Cap data length to inline buffer size */
    if (data_len > UTSM_IPC_MSG_DATA_SIZE) {
        data_len = UTSM_IPC_MSG_DATA_SIZE;
    }

    /* Build the message */
    struct utsm_ipc_msg msg;
    msg.type = msg_type;
    msg.seq = g_send_seq++;
    msg.data_len = data_len;
    msg.flags = 0;

    /* Copy data if present */
    if (data && data_len > 0) {
        const u8 *src = (const u8 *)data;
        for (u32 i = 0; i < data_len; i++) {
            msg.data[i] = src[i];
        }
    }

    /* Enqueue into utsm_to_linux ring */
    if (utsm_ipc_ring_push(&g_shm->utsm_to_linux, &msg) != 0) {
        log_warn("[IPC] send: ring full, dropping message");
        return -1;
    }

    g_send_count++;
    return 0;
}

int ipc_shm_recv(struct utsm_ipc_msg *msg_out) {
    if (!g_shm_ready || !g_shm || !msg_out) {
        return -1;
    }

    if (utsm_ipc_ring_pop(&g_shm->linux_to_utsm, msg_out) != 0) {
        return -1;  /* empty */
    }

    g_recv_count++;
    return 0;
}

int ipc_shm_has_pending(void) {
    if (!g_shm_ready || !g_shm) return 0;
    return !utsm_ipc_ring_empty(&g_shm->linux_to_utsm);
}

void ipc_shm_notify_linux(void) {
    /* Phase 1.3: log only.
     * Phase 1.4+: inject a guest IRQ (via VMCS VM-entry interruption info)
     * to notify Linux that a message is waiting in the utsm_to_linux ring. */
    log_info("[IPC] notify_linux (IPI injection not yet implemented)");
}

/* ===== Debug ===== */

void ipc_shm_dump_stats(void) {
    if (!g_shm_ready) {
        log_info("[IPC] not initialized");
        return;
    }

    log_hex64("[IPC] utsm_ready=", g_shm->header.utsm_ready);
    log_hex64("[IPC] linux_ready=", g_shm->header.linux_ready);
    log_hex64("[IPC] utsm_to_linux head=", g_shm->utsm_to_linux.head);
    log_hex64("[IPC] utsm_to_linux tail=", g_shm->utsm_to_linux.tail);
    log_hex64("[IPC] linux_to_utsm head=", g_shm->linux_to_utsm.head);
    log_hex64("[IPC] linux_to_utsm tail=", g_shm->linux_to_utsm.tail);
    log_hex64("[IPC] send_count=", g_send_count);
    log_hex64("[IPC] recv_count=", g_recv_count);
}
