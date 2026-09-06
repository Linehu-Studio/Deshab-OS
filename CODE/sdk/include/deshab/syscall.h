/* deshab/syscall.h - experimental native syscall ABI
 *
 * This header only reserves numbers and emits the x86_64 SYSCALL instruction.
 * Deshab does not currently install a native syscall entry point or dispatcher.
 * Calling these wrappers on a current image is unsupported and may fault.
 */
#ifndef DESHAB_SYSCALL_H
#define DESHAB_SYSCALL_H

#include "types.h"

#define DESHAB_SYSCALL_ABI_MAJOR 1u
#define DESHAB_SYSCALL_ABI_MINOR 0u

/*
 * Reserved v1 number ranges:
 *   0x0000-0x001f  core
 *   0x0100-0x011f  native capability IPC
 *   0x0120-0x013f  task/scheduler
 *   0x0140-0x015f  memory
 *
 * Values are explicit and must never be renumbered after implementation ships.
 */
typedef enum deshab_syscall_number {
    DESHAB_SYS_ABI_QUERY         = 0x0000,
    DESHAB_SYS_EXIT              = 0x0001,
    DESHAB_SYS_YIELD             = 0x0002,
    DESHAB_SYS_CLOCK_MONOTONIC   = 0x0003,
    DESHAB_SYS_DEBUG_WRITE       = 0x0004,

    DESHAB_SYS_IPC_QUEUE_CREATE  = 0x0100,
    DESHAB_SYS_IPC_QUEUE_DESTROY = 0x0101,
    DESHAB_SYS_IPC_SEND          = 0x0102,
    DESHAB_SYS_IPC_RECEIVE       = 0x0103,
    DESHAB_SYS_IPC_CALL          = 0x0104,
    DESHAB_SYS_IPC_REPLY         = 0x0105,
    DESHAB_SYS_IPC_WAIT          = 0x0106,
    DESHAB_SYS_IPC_SIGNAL        = 0x0107,

    DESHAB_SYS_TASK_CREATE       = 0x0120,
    DESHAB_SYS_TASK_EXIT         = 0x0121,
    DESHAB_SYS_TASK_SET_PRIORITY = 0x0122,
    DESHAB_SYS_TASK_SET_AFFINITY = 0x0123,

    DESHAB_SYS_MEM_MAP           = 0x0140,
    DESHAB_SYS_MEM_UNMAP         = 0x0141,
    DESHAB_SYS_MEM_PROTECT       = 0x0142
} deshab_syscall_number;

/*
 * Proposed x86_64 register ABI:
 *   RAX = number
 *   RDI, RSI, RDX, R10, R8, R9 = arguments 0..5
 *   RAX = signed result (negative values are errors)
 *   RCX and R11 are destroyed by SYSCALL
 */
#if (defined(__x86_64__) || defined(_M_X64)) && \
    (defined(__clang__) || defined(__GNUC__))

#define DESHAB_SYSCALL_INLINE_AVAILABLE 1

static inline i64 deshab_syscall6(u64 number, u64 arg0, u64 arg1, u64 arg2,
                                  u64 arg3, u64 arg4, u64 arg5) {
    register u64 syscall_arg3 __asm__("r10") = arg3;
    register u64 syscall_arg4 __asm__("r8") = arg4;
    register u64 syscall_arg5 __asm__("r9") = arg5;
    u64 result;

    __asm__ volatile(
        "syscall"
        : "=a"(result)
        : "a"(number), "D"(arg0), "S"(arg1), "d"(arg2),
          "r"(syscall_arg3), "r"(syscall_arg4), "r"(syscall_arg5)
        : "rcx", "r11", "memory", "cc");

    return (i64)result;
}

static inline i64 deshab_syscall0(u64 number) {
    return deshab_syscall6(number, 0, 0, 0, 0, 0, 0);
}

static inline i64 deshab_syscall1(u64 number, u64 arg0) {
    return deshab_syscall6(number, arg0, 0, 0, 0, 0, 0);
}

static inline i64 deshab_syscall2(u64 number, u64 arg0, u64 arg1) {
    return deshab_syscall6(number, arg0, arg1, 0, 0, 0, 0);
}

static inline i64 deshab_syscall3(u64 number, u64 arg0, u64 arg1, u64 arg2) {
    return deshab_syscall6(number, arg0, arg1, arg2, 0, 0, 0);
}

static inline i64 deshab_syscall4(u64 number, u64 arg0, u64 arg1, u64 arg2,
                                  u64 arg3) {
    return deshab_syscall6(number, arg0, arg1, arg2, arg3, 0, 0);
}

static inline i64 deshab_syscall5(u64 number, u64 arg0, u64 arg1, u64 arg2,
                                  u64 arg3, u64 arg4) {
    return deshab_syscall6(number, arg0, arg1, arg2, arg3, arg4, 0);
}

#else

#define DESHAB_SYSCALL_INLINE_AVAILABLE 0

#endif

#endif /* DESHAB_SYSCALL_H */
