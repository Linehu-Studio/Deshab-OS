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
#include <utsm/block.h>
#include <ipc_proto.h>
#include "../../tools/fat32_io.h"

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

/* ===== Phase 3 文件传输（FILE_LIST / FILE_READ） =====
 *
 * 同步请求-响应模型（与 exec 相同）：
 *   1. 构建 ipc_file_request（指定 payload_pool 写入偏移与容量）
 *   2. ipc_shm_send 入队 → linux_resume 唤醒 daemon
 *   3. daemon 把批量数据直接写进 payload_pool，回 FILE_RESPONSE 后 HLT
 *   4. 从 ring 取出响应头，按 data_len 从 payload_pool 拷出数据
 */

/* 发送文件请求并等待响应（公共子流程）。
 *   msg_type : UTSM_MSG_FILE_LIST_REQUEST / UTSM_MSG_FILE_READ_REQUEST
 *   path     : Linux 绝对路径
 *   offset   : FILE_READ 的文件偏移（LIST 传 0）
 *   want_cap : 期望读取的最大字节数（会钳制到 payload_pool 容量）
 * 成功时 resp 填充并返回 0；失败返回负值（-2 发送失败, -3 resume 失败,
 * -4 无响应）。 */
static int lxc_file_roundtrip(u32 msg_type, const char *path, u64 offset,
                              u32 want_cap, struct ipc_file_response *resp) {
    if (!lxc_is_available()) return -1;
    if (!path || !resp) return -1;

    u32 pool_cap = ipc_shm_payload_capacity();
    if (want_cap > pool_cap) want_cap = pool_cap;

    struct ipc_file_request req;
    my_memzero(&req, sizeof(req));
    req.pool_offset = 0;               /* 独占使用整个 pool（同步模型，无并发） */
    req.pool_capacity = want_cap;
    req.file_offset = offset;

    u64 plen = my_strlen(path);
    if (plen >= UTSM_FILE_PATH_MAX) plen = UTSM_FILE_PATH_MAX - 1;
    my_memcpy(req.path, path, plen);
    req.path[plen] = 0;

    if (ipc_shm_send(msg_type, &req, sizeof(req)) != 0) {
        log_warn("[LNXC] file: failed to enqueue request");
        return -2;
    }

    int r = linux_resume();
    if (r != 0) {
        log_hex64("[LNXC] file: linux_resume failed=", (u64)(i64)r);
        return -3;
    }

    /* 从 ring 中取 FILE_RESPONSE（忽略其间可能残留的 EXEC_* 消息） */
    for (int i = 0; i < 256; i++) {
        struct utsm_ipc_msg msg;
        if (ipc_shm_recv(&msg) != 0) break;  /* ring 空 */
        if (msg.type == UTSM_MSG_FILE_RESPONSE &&
            msg.data_len >= sizeof(struct ipc_file_response)) {
            my_memcpy(resp, msg.data, sizeof(*resp));
            return 0;
        }
    }
    return -4;  /* 无响应 */
}

/* 从 payload_pool 拷出数据到调用方缓冲。返回拷出的字节数。 */
static u64 lxc_pool_copyout(char *buf, u64 cap, u32 data_len) {
    if (!buf || cap == 0 || data_len == 0) return 0;
    u8 *pool = (u8 *)ipc_shm_payload_ptr(0);
    if (!pool) return 0;
    u64 n = data_len;
    if (n > cap) n = cap;
    if (n > ipc_shm_payload_capacity()) n = ipc_shm_payload_capacity();
    my_memcpy(buf, pool, n);
    return n;
}

static int lxc_file_list(const char *path, char *buf, u64 cap, u64 *out_len) {
    struct ipc_file_response resp;
    /* 请求的容量取调用方缓冲与 pool 容量的较小值 */
    u32 want = (cap > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)cap;
    int rc = lxc_file_roundtrip(UTSM_MSG_FILE_LIST_REQUEST, path, 0, want, &resp);
    if (rc != 0) {
        if (out_len) *out_len = 0;
        return rc;
    }
    if (resp.status != UTSM_FILE_OK) {
        if (out_len) *out_len = 0;
        log_hex64("[LNXC] file_list: linux side error=", resp.status);
        return -5;
    }
    u64 n = lxc_pool_copyout(buf, cap, resp.data_len);
    if (out_len) *out_len = n;
    /* Linux 侧写出的数据超过调用方缓冲 → 截断提示 */
    if (resp.data_len > n) return -6;
    return 0;
}

static int lxc_file_read(const char *path, u64 offset, char *buf, u64 cap,
                         u64 *out_len, u64 *out_total) {
    struct ipc_file_response resp;
    u32 want = (cap > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (u32)cap;
    int rc = lxc_file_roundtrip(UTSM_MSG_FILE_READ_REQUEST, path, offset, want, &resp);
    if (rc != 0) {
        if (out_len) *out_len = 0;
        if (out_total) *out_total = 0;
        return rc;
    }
    if (out_total) *out_total = resp.total_size;
    if (resp.status != UTSM_FILE_OK) {
        if (out_len) *out_len = 0;
        log_hex64("[LNXC] file_read: linux side error=", resp.status);
        return -5;
    }
    u64 n = lxc_pool_copyout(buf, cap, resp.data_len);
    if (out_len) *out_len = n;
    if (resp.data_len > n) return -6;
    return 0;
}

/* 写入 Linux 文件：把调用方数据先搬进 payload_pool，再走同步请求-响应。
 * 单次最多搬运 payload pool 容量（~1MB），大文件由调用方循环推进 offset。 */
static int lxc_file_write(const char *path, u64 offset, const char *buf,
                          u64 len, u64 *out_written) {
    if (!lxc_is_available()) return -1;
    if (!path || (!buf && len > 0)) return -1;

    u32 pool_cap = ipc_shm_payload_capacity();
    u64 n = len;
    if (n > pool_cap) n = pool_cap;

    if (n > 0) {
        u8 *pool = (u8 *)ipc_shm_payload_ptr(0);
        if (!pool) return -1;
        my_memcpy(pool, buf, n);
        utsm_ipc_mb();  /* 保证 pool 数据先于请求对 guest 可见 */
    }

    /* 复用公共子流程：WRITE 时 pool_capacity 字段表示已 staged 的字节数 */
    struct ipc_file_response resp;
    int rc = lxc_file_roundtrip(UTSM_MSG_FILE_WRITE_REQUEST, path, offset,
                                (u32)n, &resp);
    if (rc != 0) {
        if (out_written) *out_written = 0;
        return rc;
    }
    if (resp.status != UTSM_FILE_OK) {
        if (out_written) *out_written = 0;
        log_hex64("[LNXC] file_write: linux side error=", resp.status);
        return -5;
    }
    if (out_written) *out_written = resp.data_len;
    return 0;
}

/* ===== UTSM FAT32 系统盘接入（file_pull / file_push / hypercall 共用） =====
 *
 * 复用 tools/fat32_io.h（static inline 库，仅根目录、单文件 ≤256KB），
 * block 读写函数指针来自 UTSM block 注册表（block_get_api()，
 * 等价于 DSK 侧 dkm_kernel_api +0xA8 block_read / +0x18 block_write）。 */

static int g_f32_inited;   /* 1 = f32_init 已完成 */

/* 惰性初始化 FAT32 库。返回 0 可用，-9 无块设备。 */
static int lxc_f32_ensure(void) {
    const dkm_block_api *api = block_get_api();
    if (!api || !api->read || !api->write) return -9;
    if (api->device_count() == 0) return -9;
    if (!g_f32_inited) {
        f32_init(api->read, api->write);
        g_f32_inited = 1;
    }
    return 0;
}

int lxc_f32_read_file(const char *name, u8 **out_data, u32 *out_size) {
    if (!name || !out_data || !out_size) return -10;
    int rc = lxc_f32_ensure();
    if (rc != 0) return rc;
    char n11[11];
    if (f32_name_to_83(name, n11) != 0) return -10;
    int fr = f32_read_root_file(n11, out_data, out_size);
    if (fr == -5) return -11;   /* 根目录中未找到 */
    if (fr != 0) return -12;    /* 磁盘 / 格式错误 */
    return 0;
}

int lxc_f32_write_file(const char *name, const u8 *data, u32 size) {
    if (!name || (!data && size > 0)) return -10;
    int rc = lxc_f32_ensure();
    if (rc != 0) return rc;
    char n11[11];
    if (f32_name_to_83(name, n11) != 0) return -10;
    if (f32_write_root_file(n11, data, size) != 0) return -12;
    return 0;
}

/* pull 暂存缓冲：与 fat32_io 单文件上限一致（256KB） */
static u8 g_pull_stage[262144];

/* 跨内核拉取：Linux 文件 → UTSM FAT32 根目录。
 * 先经 IPC 分块读完整文件到暂存缓冲（失败则不写盘，避免半成品），
 * 再一次 f32_write_root_file 落盘。 */
static int lxc_file_pull(const char *linux_path, const char *fat32_name,
                         u64 *out_size) {
    if (!lxc_is_available()) return -1;
    if (!linux_path || !fat32_name) return -1;
    int rc = lxc_f32_ensure();
    if (rc != 0) { if (out_size) *out_size = 0; return rc; }

    u64 off = 0;
    u64 total = 0;
    for (;;) {
        u64 got = 0;
        u64 space = (u64)sizeof(g_pull_stage) - off;
        if (space == 0) {
            if (out_size) *out_size = 0;
            return -7;   /* 超过 256KB 暂存上限 */
        }
        rc = lxc_file_read(linux_path, off, (char *)g_pull_stage + off,
                           space, &got, &total);
        if (rc != 0) { if (out_size) *out_size = 0; return rc; }
        off += got;
        if (got == 0 || off >= total) break;
    }

    rc = lxc_f32_write_file(fat32_name, g_pull_stage, (u32)off);
    if (rc != 0) { if (out_size) *out_size = 0; return rc; }
    if (out_size) *out_size = off;
    return 0;
}

/* 跨内核推送：UTSM FAT32 根目录 → Linux 文件（整文件替换）。
 * f32_read_root_file 读出后经 lxc_file_write 按 pool 容量分块写入；
 * 首块 offset=0 → daemon 侧 O_TRUNC，实现整文件替换语义。 */
static int lxc_file_push(const char *fat32_name, const char *linux_path,
                         u64 *out_size) {
    if (!lxc_is_available()) return -1;
    if (!fat32_name || !linux_path) return -1;

    u8 *data = 0;
    u32 size = 0;
    int rc = lxc_f32_read_file(fat32_name, &data, &size);
    if (rc != 0) { if (out_size) *out_size = 0; return rc; }

    u32 pool_cap = ipc_shm_payload_capacity();
    u64 off = 0;
    for (;;) {
        u64 remain = (u64)size - off;
        u64 chunk = remain > pool_cap ? pool_cap : remain;
        u64 written = 0;
        rc = lxc_file_write(linux_path, off, (const char *)data + off,
                            chunk, &written);
        if (rc != 0) { if (out_size) *out_size = off; return rc; }
        if (written != chunk) {   /* 短写：按 I/O 错误处理 */
            if (out_size) *out_size = off + written;
            return -5;
        }
        off += written;
        if (off >= size) break;
    }
    if (out_size) *out_size = off;
    return 0;
}

/* ===== 服务表 ===== */

static const linux_compat_service g_lxc_service = {
    LINUX_COMPAT_MAGIC,
    lxc_is_available,
    lxc_exec,
    lxc_file_list,
    lxc_file_read,
    lxc_status,
    lxc_file_write,
    lxc_file_pull,
    lxc_file_push,
};

const linux_compat_service *linux_compat_get_service(void) {
    return &g_lxc_service;
}
