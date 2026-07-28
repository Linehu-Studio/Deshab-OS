#ifndef UTSM_IPC_PROTO_H
#define UTSM_IPC_PROTO_H

/*
 * UTSM ↔ Linux IPC Protocol — shared between both kernels.
 *
 * This header is included by:
 *   - UTSM kernel (CODE/UTSM/vmm/ipc_shm.c, hypercall.c)
 *   - Linux kernel module (CODE/linux/patches/utsm-ipc.patch → drivers/utsm/)
 *
 * Communication channels:
 *   1. Shared memory ring buffer (async messages, bidirectional)
 *   2. VMCALL hypercall (sync request/response)
 *   3. IPI notification (UTSM → Linux via guest IRQ injection)
 *
 * Shared memory layout (at GPA 0x04000000, 1MB total):
 *   0x04000000: utsm_ipc_shm_header (64 bytes)
 *   0x04000040: utsm_ipc_ring utsm_to_linux (UTSM writes, Linux reads)
 *   0x04001040: utsm_ipc_ring linux_to_utsm (Linux writes, UTSM reads)
 *   0x04002040: payload pool (for large data transfers)
 */

#include <stdint.h>

/* ===== Constants ===== */

#define UTSM_IPC_MAGIC          0x5554534D48430000ULL  /* "UTSMHC\00\00" */
#define UTSM_IPC_VERSION        1

#define UTSM_IPC_SHM_GPA        0x04000000ULL
#define UTSM_IPC_SHM_SIZE       (1024 * 1024)          /* 1MB */

#define UTSM_IPC_QUEUE_DEPTH    64                     /* messages per ring */
#define UTSM_IPC_MSG_DATA_SIZE  240                    /* inline data per message */
#define UTSM_IPC_PAYLOAD_SIZE   (UTSM_IPC_SHM_SIZE - 2 * 1024 - 64) /* ~1MB */

/* ===== Hypercall operations ===== */

enum utsm_hcall_op {
    UTSM_HCALL_PING          = 0x0001,  /* Simple ping (returns PONG) */
    UTSM_HCALL_HELLO         = 0x0002,  /* Hello handshake */
    UTSM_HCALL_SHM_INFO      = 0x0010,  /* Query shared memory GPA + size */
    UTSM_HCALL_CAP_VALIDATE  = 0x0020,  /* Capability validation */
    UTSM_HCALL_UTRW_READ     = 0x0030,  /* Sealed memory read (UTRW) */
    UTSM_HCALL_UTRW_WRITE    = 0x0031,  /* Sealed memory write (UTRW) */
    UTSM_HCALL_DRR_CHECKPOINT= 0x0040,  /* Trigger DRR checkpoint */
    UTSM_HCALL_CONSOLE_WRITE = 0x0050,  /* Write to UTSM serial console */
    UTSM_HCALL_CONSOLE_READ  = 0x0051,  /* Read from UTSM serial console */
};

/* Hypercall return codes */
#define UTSM_HCALL_OK           0
#define UTSM_HCALL_INVAL        (-1)
#define UTSM_HCALL_PERM         (-2)
#define UTSM_HCALL_NOMEM        (-3)
#define UTSM_HCALL_NOSYS        (-4)

/* ===== Message types ===== */

enum utsm_ipc_msg_type {
    UTSM_MSG_NONE     = 0,
    UTSM_MSG_HELLO    = 1,    /* Initial handshake */
    UTSM_MSG_HELLO_ACK= 2,    /* Handshake acknowledgment */
    UTSM_MSG_PING     = 3,    /* Ping request */
    UTSM_MSG_PONG     = 4,    /* Pong response */
    UTSM_MSG_DATA     = 5,    /* Generic data message */
    UTSM_MSG_ACK      = 6,    /* Acknowledgment */
    UTSM_MSG_NACK     = 7,    /* Negative acknowledgment */
    UTSM_MSG_SHUTDOWN = 8,    /* Shutdown notification */
    /* Linux compat exec protocol (UTSM → Linux → UTSM) */
    UTSM_MSG_EXEC_REQUEST = 16, /* UTSM→Linux: exec a program (payload=ipc_exec_request) */
    UTSM_MSG_EXEC_STDOUT  = 17, /* Linux→UTSM: stdout chunk (payload=raw bytes) */
    UTSM_MSG_EXEC_STDERR  = 18, /* Linux→UTSM: stderr chunk (payload=raw bytes) */
    UTSM_MSG_EXEC_EXIT    = 19, /* Linux→UTSM: process exited (payload=ipc_exec_exit) */
    UTSM_MSG_EXEC_READY   = 20, /* Linux→UTSM: daemon ready to accept exec requests */
};

/* ===== Linux compat exec payload structures =====
 *
 * Flow:
 *   1. UTSM writes UTSM_MSG_EXEC_REQUEST into utsm_to_linux ring,
 *      then calls linux_resume() to wake the parked Linux guest.
 *   2. Linux daemon reads the request, fork+exec's the program,
 *      streams stdout/stderr back via UTSM_MSG_EXEC_STDOUT/STDERR.
 *   3. Linux daemon writes UTSM_MSG_EXEC_EXIT with exit code, then HLTs (park).
 *   4. UTSM reads the responses from linux_to_utsm ring, returns to caller.
 *
 * All strings are NUL-terminated. argv_blob contains argv[0..argc-1] each
 * NUL-terminated, concatenated. path is the program path to execute. */

#define UTSM_EXEC_PATH_MAX   128
#define UTSM_EXEC_ARGV_MAX   96   /* total argv blob size */
#define UTSM_EXEC_MAX_ARGS   16

struct ipc_exec_request {
    uint32_t argc;                                   /* number of arguments */
    uint32_t flags;                                  /* reserved (0) */
    char     path[UTSM_EXEC_PATH_MAX];               /* program path (NUL-terminated) */
    char     argv_blob[UTSM_EXEC_ARGV_MAX];          /* argv strings, each NUL-terminated */
} __attribute__((packed));

struct ipc_exec_exit {
    uint32_t exit_code;                              /* process exit status */
    uint32_t reserved;
} __attribute__((packed));

/* ===== Ring buffer structures ===== */

/* Single IPC message (256 bytes, cache-line aligned) */
struct utsm_ipc_msg {
    uint32_t type;                     /* enum utsm_ipc_msg_type */
    uint32_t seq;                      /* Sequence number (monotonic) */
    uint32_t data_len;                 /* Actual bytes in data[] */
    uint32_t flags;                    /* Reserved for future use */
    uint8_t  data[UTSM_IPC_MSG_DATA_SIZE]; /* Inline payload */
} __attribute__((packed));

/* Ring buffer: single-producer, single-consumer queue.
 *
 * Producer writes at tail, then increments tail (with memory barrier).
 * Consumer reads at head, then increments head (with memory barrier).
 *
 * Empty when head == tail.
 * Full when (tail + 1) % capacity == head. */
struct utsm_ipc_ring {
    volatile uint32_t head;            /* Consumer updates (Linux or UTSM) */
    volatile uint32_t tail;            /* Producer updates (UTSM or Linux) */
    uint32_t capacity;                 /* Max messages (UTSM_IPC_QUEUE_DEPTH) */
    uint32_t reserved;
    struct utsm_ipc_msg messages[UTSM_IPC_QUEUE_DEPTH];
} __attribute__((packed));

/* Shared memory header */
struct utsm_ipc_shm_header {
    uint64_t magic;                    /* UTSM_IPC_MAGIC */
    uint32_t version;                  /* UTSM_IPC_VERSION */
    uint32_t header_size;              /* sizeof(struct utsm_ipc_shm_header) */
    uint64_t shm_phys;                 /* Physical address of SHM region */
    uint64_t shm_size;                 /* Total SHM size */
    volatile uint32_t utsm_ready;      /* UTSM sets to 1 when initialized */
    volatile uint32_t linux_ready;     /* Linux sets to 1 when initialized */
    uint8_t  reserved[24];
} __attribute__((packed));

/* Full shared memory region */
struct utsm_ipc_shm {
    struct utsm_ipc_shm_header header;
    struct utsm_ipc_ring utsm_to_linux;   /* UTSM writes, Linux reads */
    struct utsm_ipc_ring linux_to_utsm;   /* Linux writes, UTSM reads */
    uint8_t payload_pool[UTSM_IPC_PAYLOAD_SIZE];
} __attribute__((packed));

/* ===== Ring buffer operations (inline, usable by both kernels) ===== */

/* Memory barriers — adapted for each kernel's environment.
 * UTSM: use compiler barriers (__asm__ volatile("" ::: "memory"))
 * Linux: use smp_store_release / smp_load_acquire
 *
 * For simplicity, we use full memory barriers here. */

static inline void utsm_ipc_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* Initialize a ring buffer */
static inline void utsm_ipc_ring_init(struct utsm_ipc_ring *ring) {
    ring->head = 0;
    ring->tail = 0;
    ring->capacity = UTSM_IPC_QUEUE_DEPTH;
    ring->reserved = 0;
    utsm_ipc_mb();
}

/* Try to enqueue a message. Returns 0 on success, -1 if full. */
static inline int utsm_ipc_ring_push(struct utsm_ipc_ring *ring,
                                     const struct utsm_ipc_msg *msg) {
    uint32_t tail = ring->tail;
    uint32_t next_tail = (tail + 1) % ring->capacity;

    if (next_tail == ring->head) {
        return -1;  /* Full */
    }

    ring->messages[tail] = *msg;
    utsm_ipc_mb();  /* Ensure message is visible before updating tail */
    ring->tail = next_tail;

    return 0;
}

/* Try to dequeue a message. Returns 0 on success, -1 if empty. */
static inline int utsm_ipc_ring_pop(struct utsm_ipc_ring *ring,
                                    struct utsm_ipc_msg *msg) {
    uint32_t head = ring->head;

    if (head == ring->tail) {
        return -1;  /* Empty */
    }

    *msg = ring->messages[head];
    utsm_ipc_mb();  /* Ensure we've read the message before updating head */
    ring->head = (head + 1) % ring->capacity;

    return 0;
}

/* Check if ring is empty */
static inline int utsm_ipc_ring_empty(const struct utsm_ipc_ring *ring) {
    return ring->head == ring->tail;
}

/* Check if ring is full */
static inline int utsm_ipc_ring_full(const struct utsm_ipc_ring *ring) {
    uint32_t next_tail = (ring->tail + 1) % ring->capacity;
    return next_tail == ring->head;
}

/* ===== Hypercall ABI (Linux → UTSM) =====
 *
 * Linux calls VMCALL with:
 *   RAX = UTSM_IPC_MAGIC | op
 *   RDI = arg0
 *   RSI = arg1
 *   RDX = arg2
 *
 * UTSM handles the VMCALL VM-Exit and returns:
 *   RAX = return value (UTSM_HCALL_OK or error)
 *
 * The magic in RAX distinguishes UTSM hypercalls from other VMCALL users.
 */

#define UTSM_HCALL_MAGIC        0x5554534D48430000ULL  /* "UTSMHC\00\00" */

static inline long utsm_hcall(uint64_t op, uint64_t a0, uint64_t a1, uint64_t a2) {
    long ret;
    __asm__ volatile("vmcall"
                     : "=a"(ret)
                     : "a"(UTSM_HCALL_MAGIC | op), "D"(a0), "S"(a1), "d"(a2)
                     : "memory");
    return ret;
}

/* Convenience hypercall wrappers */
static inline long utsm_hcall_ping(void) {
    return utsm_hcall(UTSM_HCALL_PING, 0, 0, 0);
}

static inline long utsm_hcall_hello(void) {
    return utsm_hcall(UTSM_HCALL_HELLO, 0, 0, 0);
}

static inline long utsm_hcall_shm_info(uint64_t *gpa_out, uint64_t *size_out) {
    uint64_t gpa = 0, size = 0;
    long ret = utsm_hcall(UTSM_HCALL_SHM_INFO, (uint64_t)&gpa, (uint64_t)&size, 0);
    if (ret == 0) {
        if (gpa_out) *gpa_out = gpa;
        if (size_out) *size_out = size;
    }
    return ret;
}

static inline long utsm_hcall_console_write(const char *buf, uint64_t len) {
    return utsm_hcall(UTSM_HCALL_CONSOLE_WRITE, (uint64_t)buf, len, 0);
}

#endif /* UTSM_IPC_PROTO_H */
