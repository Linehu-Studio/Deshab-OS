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
 * Shared memory layout (at GPA 0x04000000, 1MB total, packed struct):
 *   0x04000000: utsm_ipc_shm_header (64 bytes)
 *   0x04000040: utsm_ipc_ring utsm_to_linux (UTSM writes, Linux reads, 16400 B)
 *   0x04004050: utsm_ipc_ring linux_to_utsm (Linux writes, UTSM reads, 16400 B)
 *   0x04008060: payload pool (UTSM_IPC_PAYLOAD_SIZE bytes, ends exactly at +1MB)
 */

/* Kernel-space uses kernel types, user-space uses <stdint.h>.
 * UTSM kernel defines __UTSM_KERNEL__, Linux kernel auto-defines __KERNEL__. */
#if defined(__KERNEL__)
#include <linux/types.h>
/* Linux kernel uses __u32/__u64/__u8; map to stdint names for shared structs */
typedef __u8  uint8_t;
typedef __u32 uint32_t;
typedef __u64 uint64_t;
#elif defined(__UTSM_KERNEL__)
#include <utsm/types.h>
#else
#include <stdint.h>
#endif

/* ===== Constants ===== */

#define UTSM_IPC_MAGIC          0x5554534D48430000ULL  /* "UTSMHC\00\00" */
#define UTSM_IPC_VERSION        1

#define UTSM_IPC_SHM_GPA        0x04000000ULL
#define UTSM_IPC_SHM_SIZE       (1024 * 1024)          /* 1MB */

#define UTSM_IPC_QUEUE_DEPTH    64                     /* messages per ring */
#define UTSM_IPC_MSG_DATA_SIZE  240                    /* inline data per message */

/* Derived region sizes (packed layout, both kernels must agree):
 *   message  = 16-byte header + inline data
 *   ring     = 16-byte header + QUEUE_DEPTH messages
 *   SHM      = 64-byte header + 2 rings + payload pool = exactly 1MB */
#define UTSM_IPC_MSG_SIZE       (16 + UTSM_IPC_MSG_DATA_SIZE)                 /* 256 */
#define UTSM_IPC_RING_SIZE      (16 + UTSM_IPC_QUEUE_DEPTH * UTSM_IPC_MSG_SIZE) /* 16400 */
#define UTSM_IPC_PAYLOAD_SIZE   (UTSM_IPC_SHM_SIZE - 64 - 2 * UTSM_IPC_RING_SIZE) /* 1015712 */

/* ===== Hypercall operations ===== */

enum utsm_hcall_op {
    UTSM_HCALL_PING          = 0x0001,  /* Simple ping (returns PONG) */
    UTSM_HCALL_HELLO         = 0x0002,  /* Hello handshake */
    UTSM_HCALL_PARK          = 0x0003,  /* Park vCPU back to DSK (not HLT) */
    UTSM_HCALL_SHM_INFO      = 0x0010,  /* Query shared memory GPA + size */
    UTSM_HCALL_CAP_VALIDATE  = 0x0020,  /* Capability validation */
    UTSM_HCALL_UTRW_READ     = 0x0030,  /* Sealed memory read (UTRW) */
    UTSM_HCALL_UTRW_WRITE    = 0x0031,  /* Sealed memory write (UTRW) */
    UTSM_HCALL_DRR_CHECKPOINT= 0x0040,  /* Trigger DRR checkpoint */
    UTSM_HCALL_CONSOLE_WRITE = 0x0050,  /* Write to UTSM serial console */
    UTSM_HCALL_CONSOLE_READ  = 0x0051,  /* Read from UTSM serial console */
    /* Phase 3 file transfer: Linux guest ↔ UTSM FAT32 disk (payload_pool as
     * the bulk data buffer; see "Linux ↔ UTSM FAT32 hypercalls" below) */
    UTSM_HCALL_FILE_READ     = 0x0060,  /* Read UTSM FAT32 root file into payload_pool */
    UTSM_HCALL_FILE_WRITE    = 0x0061,  /* Write payload_pool content to UTSM FAT32 root file */
    /* VSCode integration Phase 1: query graphics surface pool (GPA + size).
     * The surface pool is a separate DMA region EPT-mapped into the Linux
     * guest at LINUX_GUEST_SURFACE_GPA. Linux mmaps it via /dev/utsm and
     * exposes it to X server as the scanout framebuffer (Phase 3 virtio-gpu
     * shadow buffer lives here; guest writes → host reads for blit). */
    UTSM_HCALL_SURFACE_INFO  = 0x0070,  /* Query graphics surface pool GPA + size */
};

/* Hypercall return codes */
#define UTSM_HCALL_OK           0
#define UTSM_HCALL_INVAL        (-1)
#define UTSM_HCALL_PERM         (-2)
#define UTSM_HCALL_NOMEM        (-3)
#define UTSM_HCALL_NOSYS        (-4)
#define UTSM_HCALL_NOENT        (-5)   /* file not found */
#define UTSM_HCALL_IO           (-6)   /* disk / generic I/O error */

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
    /* Linux compat file transfer protocol (payload via payload_pool) */
    UTSM_MSG_FILE_LIST_REQUEST = 24, /* UTSM→Linux: list directory (payload=ipc_file_request) */
    UTSM_MSG_FILE_READ_REQUEST = 25, /* UTSM→Linux: read file chunk (payload=ipc_file_request) */
    UTSM_MSG_FILE_RESPONSE     = 26, /* Linux→UTSM: result header (payload=ipc_file_response),
                                      * bulk data already in payload_pool */
    UTSM_MSG_FILE_WRITE_REQUEST= 27, /* UTSM→Linux: write file chunk (payload=ipc_file_request),
                                      * bulk data pre-staged in payload_pool by UTSM;
                                      * result via UTSM_MSG_FILE_RESPONSE */
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

/* VSCode Phase 5: exec flags (ipc_exec_request.flags) */
#define UTSM_EXEC_FLAG_ASYNC 0x00000001u  /* daemon fork+setsid+detach, replies
                                           * EXEC_EXIT(0) immediately after
                                           * spawn; no stdout capture, no wait.
                                           * Used to launch X server / VSCode
                                           * without blocking the UTSM side. */

struct ipc_exec_request {
    uint32_t argc;                                   /* number of arguments */
    uint32_t flags;                                  /* UTSM_EXEC_FLAG_* (0 = sync) */
    char     path[UTSM_EXEC_PATH_MAX];               /* program path (NUL-terminated) */
    char     argv_blob[UTSM_EXEC_ARGV_MAX];          /* argv strings, each NUL-terminated */
} __attribute__((packed));

struct ipc_exec_exit {
    uint32_t exit_code;                              /* process exit status */
    uint32_t flags;                                  /* UTSM_EXEC_EXIT_F_* (0 = 旧式 8B) */
    uint32_t stdout_len;                             /* payload_pool 内有效字节数 */
    uint32_t stdout_total;                           /* 截断前总输出字节数（诊断） */
} __attribute__((packed));

/* ipc_exec_exit.flags — 大输出/超时扩展（P3 补完）。
 * 旧 daemon 只发 8B（exit_code+reserved=0），UTSM 按 data_len>=16 且
 * flags&UTSM_EXEC_EXIT_F_POOL 判定新协议；旧式输出仍走 EXEC_STDOUT 内联。 */
#define UTSM_EXEC_EXIT_F_POOL     0x00000001u  /* stdout 经 payload_pool 传输（offset 0） */
#define UTSM_EXEC_EXIT_F_TRUNC    0x00000002u  /* 输出超 pool 容量被截断 */
#define UTSM_EXEC_EXIT_F_TIMEOUT  0x00000004u  /* 子进程超时被 SIGKILL（exit_code=124） */

/* 同步 exec daemon 侧子进程超时（秒）：park-and-resume 模型下 UTSM 在
 * linux_resume() 期间不运行，无法宿主侧看门狗，必须 daemon alarm 兜底，
 * 否则 guest 内长命令（sleep infinity 等）会永久冻结整系统。 */
#define UTSM_EXEC_TIMEOUT_SEC     15u

/* ===== Linux compat file transfer payload structures =====
 *
 * Flow (synchronous with park-and-resume, same as exec):
 *   1. UTSM writes UTSM_MSG_FILE_LIST_REQUEST / FILE_READ_REQUEST into the
 *      utsm_to_linux ring, then linux_resume() wakes the daemon.
 *   2. Linux daemon performs opendir/readdir or open/pread, writes the bulk
 *      result data DIRECTLY into payload_pool at req.pool_offset (via
 *      UTSM_IOCTL_WRITE_POOL), then enqueues UTSM_MSG_FILE_RESPONSE carrying
 *      ipc_file_response (status + data_len + total_size), then HLTs.
 *   3. UTSM drains the ring, reads the response header, then copies
 *      resp.data_len bytes out of payload_pool + req.pool_offset.
 *
 * Write flow (UTSM → Linux, UTSM_MSG_FILE_WRITE_REQUEST):
 *   1. UTSM stages the data into payload_pool at req.pool_offset (offset 0,
 *      exclusive use — the model is synchronous), sets req.pool_capacity to
 *      the staged byte count and req.file_offset to the write position.
 *   2. Linux daemon reads req.pool_capacity bytes out of payload_pool (via
 *      UTSM_IOCTL_READ_POOL) and pwrite()s them to req.path at
 *      req.file_offset. Convention: file_offset == 0 opens with O_TRUNC
 *      (a push always replaces the file); offset > 0 writes in place.
 *      The file is created with mode 0644 if missing.
 *   3. Daemon replies UTSM_MSG_FILE_RESPONSE with data_len = bytes written
 *      and total_size = resulting file size.
 *
 * payload_pool is ~1MB (UTSM_IPC_PAYLOAD_SIZE), so a single round trip can
 * move up to that much data. For larger files the caller loops with
 * increasing file_offset. Directory listings are formatted as plain text,
 * one entry per line ("name" or "name/" for directories).
 *
 * All strings are NUL-terminated. */

#define UTSM_FILE_PATH_MAX   200

/* Status codes for ipc_file_response.status */
#define UTSM_FILE_OK         0
#define UTSM_FILE_ERR_NOENT  1   /* path does not exist */
#define UTSM_FILE_ERR_IO     2   /* generic I/O error */
#define UTSM_FILE_ERR_NOTDIR 3   /* LIST on a non-directory */
#define UTSM_FILE_ERR_ISDIR  4   /* READ/WRITE on a directory */
#define UTSM_FILE_ERR_PERM   5   /* permission denied */
#define UTSM_FILE_ERR_INVAL  6   /* malformed request */
#define UTSM_FILE_ERR_NOSPC  7   /* no space left on device / file too large */

struct ipc_file_request {
    uint32_t pool_offset;                          /* payload_pool offset of bulk data */
    uint32_t pool_capacity;                        /* LIST/READ: max bytes daemon may write;
                                                    * WRITE: staged data byte count */
    uint64_t file_offset;                          /* READ: read offset; WRITE: write offset
                                                    * (0 also implies O_TRUNC); LIST: 0 */
    char     path[UTSM_FILE_PATH_MAX];             /* absolute Linux path (NUL-terminated) */
} __attribute__((packed));

struct ipc_file_response {
    uint32_t status;                               /* UTSM_FILE_* */
    uint32_t data_len;                             /* LIST/READ: bytes written into payload_pool;
                                                    * WRITE: bytes written to the file */
    uint64_t total_size;                           /* READ: total file size;
                                                    * WRITE: resulting file size; LIST: 0 */
} __attribute__((packed));

/* ===== Linux ↔ UTSM FAT32 hypercalls (UTSM_HCALL_FILE_READ/WRITE) =====
 *
 * These give the Linux guest direct access to files on the UTSM-side FAT32
 * system disk (root directory, 8.3-convertible names, whole-file semantics,
 * max 256KB per file). payload_pool at offset 0 is the bulk data buffer.
 *
 * UTSM_HCALL_FILE_READ:  a0 = GPA of NUL-terminated file name (max 63 chars)
 *                        a1 = file offset (u64)
 *                        a2 = max bytes to stage (clamped to pool capacity)
 *   UTSM reads the file via its block/FAT32 path, stages
 *   payload_pool[0..n) = file[a1 .. a1+n), and returns in RAX:
 *       low 20 bits  = n (bytes staged; pool capacity < 1MB so 20 bits fit)
 *       high bits    = total file size (RAX >> 20)
 *   Negative RAX = UTSM_HCALL_* error (INVAL bad name, NOENT missing,
 *   NOMEM pool unavailable, IO disk error). n == 0 with a1 >= size means EOF.
 *
 * UTSM_HCALL_FILE_WRITE: a0 = GPA of NUL-terminated file name (max 63 chars)
 *                        a1 = reserved (0)
 *                        a2 = byte count staged at payload_pool[0]
 *   Whole-file replace: the staged bytes become the new file content
 *   (create or replace). Returns bytes written (== a2) or a negative
 *   UTSM_HCALL_* error.
 *
 * Concurrency note: payload_pool is a single-writer channel; the guest must
 * not run these hypercalls concurrently with a ring-based file transfer. */

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

/* Compile-time guard: the packed SHM layout must fit in the 1MB region
 * (UTSM allocates exactly UTSM_IPC_SHM_SIZE bytes and EPT-maps only that). */
_Static_assert(sizeof(struct utsm_ipc_shm) <= UTSM_IPC_SHM_SIZE,
               "utsm_ipc_shm exceeds the 1MB shared memory region");

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
 *
 * Define UTSM_NO_INLINE_HCALL before including this header if you provide
 * your own hypercall wrapper implementations (e.g. Linux utsm_hcall.c).
 */

#define UTSM_HCALL_MAGIC        0x5554534D48430000ULL  /* "UTSMHC\00\00" */

#ifndef UTSM_NO_INLINE_HCALL

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

/* VSCode integration Phase 1: query graphics surface pool.
 * Returns the surface pool GPA (visible inside the Linux guest) and size.
 * Linux user-space mmaps /dev/utsm at the returned GPA to obtain a writable
 * pointer that the host can scan for blitting to the Deshab framebuffer. */
static inline long utsm_hcall_surface_info(uint64_t *gpa_out, uint64_t *size_out) {
    uint64_t gpa = 0, size = 0;
    long ret = utsm_hcall(UTSM_HCALL_SURFACE_INFO, (uint64_t)&gpa, (uint64_t)&size, 0);
    if (ret == 0) {
        if (gpa_out) *gpa_out = gpa;
        if (size_out) *size_out = size;
    }
    return ret;
}

static inline long utsm_hcall_console_write(const char *buf, uint64_t len) {
    return utsm_hcall(UTSM_HCALL_CONSOLE_WRITE, (uint64_t)buf, len, 0);
}

#endif /* UTSM_NO_INLINE_HCALL */

#endif /* UTSM_IPC_PROTO_H */
