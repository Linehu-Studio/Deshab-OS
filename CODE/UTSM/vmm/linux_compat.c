/* linux_compat.c — Deshab Linux 程序兼容层服务实现。
 *
 * 依赖 UTSM+Linux 双内核架构与 IPC 共享内存。Linux guest 作为 daemon
 * 在 VMX guest 中 park-and-resume 驻留。UTSM 侧通过 IPC ring 向 daemon
 * 发送 exec 请求，并 vmresume 唤醒 guest 执行。
 *
 * 执行流程（exec()）：
 *   1. 构建 ipc_exec_request，通过 ipc_shm_send(UTSM_MSG_EXEC_REQUEST) 入队
 *   2. 调用 linux_resume() → vmresume → Linux daemon 读取请求 → fork+exec
 *   3. daemon 通过 linux_to_utsm ring 回传 UTSM_MSG_EXEC_STDOUT/STDERR/EXIT
 *   4. daemon HLT park → linux_resume() 返回
 *   5. exec() 从 ring 读取所有响应消息，拼接 stdout，取 exit code
 *
 * 服务指针通过 dsk_boot_context.reserved[5] 暴露给 shell/cmd/desktop。
 */

#include <utsm/linux_compat.h>
#include <utsm/linux_resume.h>
#include <utsm/ipc_shm.h>
#include <utsm/vmm.h>
#include <utsm/log.h>
#include <utsm/types.h>
#include <ipc_proto.h>

/* ===== 内部辅助 ===== */

static u64 my_strlen(const char *s) {
    u64 n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static void my_memcpy(void *d, const void *s, u64 n) {
    u8 *dd = (u8 *)d;
    const u8 *ss = (const u8 *)s;
    while (n--) *dd++ = *ss++;
}

static void my_memzero(void *d, u64 n) {
    u8 *p = (u8 *)d;
    while (n--) *p++ = 0;
}

/* ===== 服务状态 ===== */

static int g_compat_ready;   /* 1 = linux_launch 已成功，服务可用 */

void linux_compat_init(void) {
    /* 在 linux_launch 成功（guest parked）后由 kernel_main 调用 */
    if (linux_is_parked() && ipc_shm_is_ready()) {
        g_compat_ready = 1;
        log_info("[LNXC] service ready (guest parked, IPC ok)");
    } else {
        g_compat_ready = 0;
        log_warn("[LNXC] service not ready (guest not parked or IPC down)");
    }
}

/* ===== 服务方法 ===== */

static int lxc_is_available(void) {
    return (g_compat_ready && linux_is_parked() && ipc_shm_is_ready()) ? 1 : 0;
}

/* 等待 Linux daemon 的响应消息。
 * linux_resume() 返回后，daemon 已 park，所有响应消息应已在 ring 中。
 * 我们循环 pop 直到拿到 EXEC_EXIT 或 ring 空（超时保护）。 */
static int lxc_drain_responses(char *out_buf, u64 out_cap, u64 *out_len,
                               u64 *exit_code) {
    u64 written = 0;
    int got_exit = 0;
    u64 exit_val = 0;

    /* 最多消费 256 条消息（防止异常情况下死循环） */
    for (int i = 0; i < 256; i++) {
        struct utsm_ipc_msg msg;
        if (ipc_shm_recv(&msg) != 0) {
            /* ring 空 */
            break;
        }

        switch (msg.type) {
        case UTSM_MSG_EXEC_STDOUT:
        case UTSM_MSG_EXEC_STDERR: {
            /* 拼接到 stdout 缓冲区（stderr 也并入，简化调用方处理） */
            u32 n = msg.data_len;
            if (out_buf && out_cap > written) {
                u32 space = (u32)(out_cap - written);
                u32 to_copy = (n < space) ? n : space;
                my_memcpy(out_buf + written, msg.data, to_copy);
                written += to_copy;
            }
            break;
        }
        case UTSM_MSG_EXEC_EXIT: {
            if (msg.data_len >= sizeof(struct ipc_exec_exit)) {
                struct ipc_exec_exit ex;
                my_memcpy(&ex, msg.data, sizeof(ex));
                exit_val = ex.exit_code;
            }
            got_exit = 1;
            break;
        }
        default:
            /* 忽略其他消息（HELLO/PONG 等） */
            break;
        }

        if (got_exit) break;
    }

    if (out_len) *out_len = written;
    if (exit_code) *exit_code = exit_val;

    return got_exit ? 0 : -4;  /* -4 = 超时无 exit 响应 */
}

static int lxc_exec(const char *path, int argc, const char *const *argv,
                    char *stdout_buf, u64 stdout_cap, u64 *stdout_len,
                    u64 *exit_code) {
    if (!lxc_is_available()) {
        return -1;
    }
    if (!path) {
        return -1;
    }

    /* 1. 构建 exec 请求 */
    struct ipc_exec_request req;
    my_memzero(&req, sizeof(req));
    req.argc = (u32)argc;
    req.flags = 0;

    u64 plen = my_strlen(path);
    if (plen >= UTSM_EXEC_PATH_MAX) plen = UTSM_EXEC_PATH_MAX - 1;
    my_memcpy(req.path, path, plen);
    req.path[plen] = 0;

    /* 拼接 argv_blob：每个 argv[i] NUL-terminated */
    u64 blob_off = 0;
    if (argc > UTSM_EXEC_MAX_ARGS) argc = UTSM_EXEC_MAX_ARGS;
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) continue;
        u64 alen = my_strlen(argv[i]);
        if (blob_off + alen + 1 > UTSM_EXEC_ARGV_MAX) break;
        my_memcpy(req.argv_blob + blob_off, argv[i], alen);
        blob_off += alen;
        req.argv_blob[blob_off++] = 0;
    }

    /* 2. 发送 EXEC_REQUEST */
    if (ipc_shm_send(UTSM_MSG_EXEC_REQUEST, &req, sizeof(req)) != 0) {
        log_warn("[LNXC] exec: failed to enqueue request");
        return -2;
    }

    /* 3. 唤醒 Linux guest 执行 */
    int r = linux_resume();
    if (r != 0) {
        log_hex64("[LNXC] exec: linux_resume failed=", (u64)(i64)r);
        return -3;
    }

    /* 4. 读取响应（daemon 已再次 park，响应在 ring 中） */
    return lxc_drain_responses(stdout_buf, stdout_cap, stdout_len, exit_code);
}

static int lxc_status(char *buf, u64 cap) {
    if (!buf || cap == 0) return 0;
    const char *s;
    if (!g_compat_ready) {
        s = "linux_compat: not initialized";
    } else if (!linux_is_parked()) {
        s = "linux_compat: guest not parked";
    } else if (!ipc_shm_is_ready()) {
        s = "linux_compat: IPC down";
    } else {
        s = "linux_compat: ready";
    }
    u64 n = my_strlen(s);
    if (n >= cap) n = cap - 1;
    my_memcpy(buf, s, n);
    buf[n] = 0;
    return (int)n;
}

/* ===== 服务表 ===== */

static const linux_compat_service g_lxc_service = {
    LINUX_COMPAT_MAGIC,
    lxc_is_available,
    lxc_exec,
    lxc_status,
};

const linux_compat_service *linux_compat_get_service(void) {
    return &g_lxc_service;
}
