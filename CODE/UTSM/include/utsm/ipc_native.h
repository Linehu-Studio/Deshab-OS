#ifndef UTSM_IPC_NATIVE_H
#define UTSM_IPC_NATIVE_H

#include <utsm/types.h>

/*
 * Experimental native Deshab capability IPC ABI.
 *
 * This is intentionally distinct from CODE/utsm-ipc/ipc_proto.h, which is a
 * UTSM <-> Linux guest transport. No native queue implementation or syscall
 * binding exists yet. All ABI fields use bounded fixed-width Deshab types;
 * pointers and implementation-owned kernel addresses never appear on wire.
 */

#define UTSM_IPC_NATIVE_MAGIC          0x44534950u /* "DSIP" */
#define UTSM_IPC_NATIVE_ABI_MAJOR      1u
#define UTSM_IPC_NATIVE_ABI_MINOR      0u

#define UTSM_IPC_NATIVE_MESSAGE_BYTES  256u
#define UTSM_IPC_NATIVE_PAYLOAD_BYTES  144u
#define UTSM_IPC_NATIVE_MAX_CAPS       4u
#define UTSM_IPC_NATIVE_MIN_DEPTH      2u
#define UTSM_IPC_NATIVE_MAX_DEPTH      4096u
#define UTSM_IPC_NATIVE_DEADLINE_NONE  (~0ULL)

/*
 * The rights field requests or describes attenuation. Authority comes from
 * the calling task's kernel capability table; copying or editing this value
 * must never create authority.
 */
typedef struct utsm_ipc_native_cap {
    u64 object_id;
    u32 generation;
    u32 rights;
} utsm_ipc_native_cap;

typedef enum utsm_ipc_native_right {
    UTSM_IPC_RIGHT_SEND      = (1u << 0),
    UTSM_IPC_RIGHT_RECEIVE   = (1u << 1),
    UTSM_IPC_RIGHT_CALL      = (1u << 2),
    UTSM_IPC_RIGHT_REPLY     = (1u << 3),
    UTSM_IPC_RIGHT_TRANSFER  = (1u << 4),
    UTSM_IPC_RIGHT_DUPLICATE = (1u << 5),
    UTSM_IPC_RIGHT_MANAGE    = (1u << 6),
    UTSM_IPC_RIGHT_WAIT      = (1u << 7),
    UTSM_IPC_RIGHT_SIGNAL    = (1u << 8)
} utsm_ipc_native_right;

#define UTSM_IPC_RIGHT_ALL ((u32)((1u << 9) - 1u))

/*
 * Queue operations are carried in a u16 ABI field. CALL creates a transaction
 * ID; REPLY must name that transaction. WAIT/SIGNAL are queue-state events,
 * not substitutes for SEND/RECEIVE.
 */
typedef enum utsm_ipc_native_queue_op {
    UTSM_IPC_QUEUE_CREATE  = 1,
    UTSM_IPC_QUEUE_DESTROY = 2,
    UTSM_IPC_QUEUE_SEND    = 3,
    UTSM_IPC_QUEUE_RECEIVE = 4,
    UTSM_IPC_QUEUE_CALL    = 5,
    UTSM_IPC_QUEUE_REPLY   = 6,
    UTSM_IPC_QUEUE_WAIT    = 7,
    UTSM_IPC_QUEUE_SIGNAL  = 8,
    UTSM_IPC_QUEUE_QUERY   = 9
} utsm_ipc_native_queue_op;

#define UTSM_IPC_MSG_F_NONBLOCK       (1u << 0)
#define UTSM_IPC_MSG_F_MOVE_CAPS      (1u << 1)
#define UTSM_IPC_MSG_F_EXPECT_REPLY   (1u << 2)
#define UTSM_IPC_MSG_F_CANCEL         (1u << 3)

typedef enum utsm_ipc_native_result {
    UTSM_IPC_NATIVE_OK              = 0,
    UTSM_IPC_NATIVE_ERR_INVALID     = -1,
    UTSM_IPC_NATIVE_ERR_ACCESS      = -2,
    UTSM_IPC_NATIVE_ERR_STALE_CAP   = -3,
    UTSM_IPC_NATIVE_ERR_FULL        = -4,
    UTSM_IPC_NATIVE_ERR_EMPTY       = -5,
    UTSM_IPC_NATIVE_ERR_CLOSED      = -6,
    UTSM_IPC_NATIVE_ERR_TIMEOUT     = -7,
    UTSM_IPC_NATIVE_ERR_CANCELLED   = -8,
    UTSM_IPC_NATIVE_ERR_UNSUPPORTED = -38
} utsm_ipc_native_result;

/*
 * Fixed 256-byte message. message_size must equal sizeof(this structure),
 * payload_length is bounded by PAYLOAD_BYTES, and capability_count is bounded
 * by MAX_CAPS. Receivers must reject nonzero reserved fields.
 */
typedef struct utsm_ipc_native_message {
    u32 magic;
    u16 abi_major;
    u16 abi_minor;

    u16 operation;       /* utsm_ipc_native_queue_op */
    u16 flags;
    u32 message_size;
    u64 transaction_id;

    utsm_ipc_native_cap endpoint;
    u32 payload_length;
    u16 capability_count;
    u16 reserved0;

    utsm_ipc_native_cap capabilities[UTSM_IPC_NATIVE_MAX_CAPS];
    u8 payload[UTSM_IPC_NATIVE_PAYLOAD_BYTES];
} utsm_ipc_native_message;

/*
 * Fixed queue-control request for a future syscall dispatcher. depth is valid
 * only for CREATE and must stay within MIN_DEPTH..MAX_DEPTH. deadline_ns is
 * an absolute monotonic deadline or UTSM_IPC_NATIVE_DEADLINE_NONE.
 */
typedef struct utsm_ipc_native_queue_request {
    u16 abi_major;
    u16 abi_minor;
    u16 operation;       /* utsm_ipc_native_queue_op */
    u16 flags;

    u32 depth;
    u32 message_size;
    utsm_ipc_native_cap queue;
    u64 deadline_ns;
} utsm_ipc_native_queue_request;

_Static_assert(sizeof(utsm_ipc_native_cap) == 16u,
               "native IPC capability ABI changed");
_Static_assert(sizeof(utsm_ipc_native_message) ==
                   UTSM_IPC_NATIVE_MESSAGE_BYTES,
               "native IPC message ABI changed");
_Static_assert(sizeof(utsm_ipc_native_queue_request) == 40u,
               "native IPC queue request ABI changed");

#endif /* UTSM_IPC_NATIVE_H */
