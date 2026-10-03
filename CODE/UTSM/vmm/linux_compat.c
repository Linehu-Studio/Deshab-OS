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
#include <utsm/panic.h>
#include <utsm/linux_resume.h>
#include <utsm/linux_loader.h>   /* VSCode Phase 5: linux_find_vscode_module */
#include <utsm/ipc_shm.h>
#include <utsm/vmm.h>
#include <utsm/virtio_mmio.h>   /* VSCode Phase 2: virtio_input_push_* */
#include <utsm/log.h>
#include <utsm/types.h>
#include <utsm/block.h>
#include <ipc_proto.h>
#include "../../tools/fat32_io.h"

/* 兼容层全局配置（由 UTSM kernel/main.c 定义） */
extern char g_compat_lib_path[];
extern char g_compat_linux_guest_path[];

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

/* 前向声明：payload_pool 拷出助手（定义在文件传输段，exec 新协议复用） */
static u64 lxc_pool_copyout(char *buf, u64 cap, u32 data_len);

/* Consume already-queued EXEC_* messages. Returns 1 if EXEC_EXIT was
 * seen, 0 if the ring is empty. stdout is accumulated across slices. */
static int lxc_consume_exec_msgs(char *out_buf, u64 out_cap, u64 *written_inout,
                                 u64 *exit_code) {
    u64 written = written_inout ? *written_inout : 0;
    int got_exit = 0;
    u64 exit_val = exit_code ? *exit_code : 0;

    for (int i = 0; i < 256; i++) {
        struct utsm_ipc_msg msg;
        if (ipc_shm_recv(&msg) != 0) break;

        switch (msg.type) {
        case UTSM_MSG_EXEC_STDOUT:
        case UTSM_MSG_EXEC_STDERR: {
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
                if (ex.flags & UTSM_EXEC_EXIT_F_POOL) {
                    written = lxc_pool_copyout(out_buf, out_cap,
                                               ex.stdout_len);
                    if (ex.flags & UTSM_EXEC_EXIT_F_TRUNC) {
                        log_warn("[LNXC] exec: output truncated at pool cap");
                        log_hex64("[LNXC] exec: total bytes=", ex.stdout_total);
                    }
                    if (ex.flags & UTSM_EXEC_EXIT_F_TIMEOUT)
                        log_warn("[LNXC] exec: child timed out, SIGKILLed");
                }
            } else if (msg.data_len >= 8) {
                u32 ec = 0;
                my_memcpy(&ec, msg.data, 4);
                exit_val = ec;
            }
            got_exit = 1;
            break;
        }
        default:
            break;
        }
        if (got_exit) break;
    }

    if (written_inout) *written_inout = written;
    if (exit_code) *exit_code = exit_val;
    return got_exit;
}

#define LXC_MAX_SLICES 256

/* linux_resume() may return on a preemption timeslice before the daemon
 * parks. Keep running slices until EXEC_EXIT (or the slice budget). */
static int lxc_wait_exec_exit(char *out_buf, u64 out_cap, u64 *out_len,
                              u64 *exit_code) {
    u64 written = 0;
    u64 exit_val = 0;
    for (int slice = 0; slice < LXC_MAX_SLICES; slice++) {
        int r = linux_resume();
        if (r != 0) {
            log_hex64("[LNXC] exec: linux_resume failed=", (u64)(i64)r);
            return -3;
        }
        if (lxc_consume_exec_msgs(out_buf, out_cap, &written, &exit_val)) {
            if (out_len) *out_len = written;
            if (exit_code) *exit_code = exit_val;
            return 0;
        }
    }
    if (out_len) *out_len = written;
    if (exit_code) *exit_code = exit_val;
    return -4;
}

/* 构建并发送 EXEC_REQUEST，唤醒 guest，然后 drain 响应。
 * flags 取 0（同步，捕获 stdout）或 UTSM_EXEC_FLAG_ASYNC（detach 立即返回）。
 * 返回 0 = 收到 EXEC_EXIT（exit_code 已写出），负值失败。 */
static int lxc_exec_internal(const char *path, int argc,
                             const char *const *argv, u32 flags,
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
    req.flags = flags;

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
        panic_full("LNX-E10 EXEC ENQUEUE FAILED", "ipc_shm_send rejected the exec request", 0);
        return -2;
    }

    /* 3. 唤醒 Linux guest；timeslice 归还时继续 resume 直到 EXEC_EXIT */
    int drc = lxc_wait_exec_exit(stdout_buf, stdout_cap, stdout_len, exit_code);
    if (drc != 0) {
        log_hex64("[LNXC] exec: no EXEC_EXIT after resume rc=", (u64)(i64)drc);
        ipc_shm_dump_stats();
    }
    return drc;
}

static int lxc_exec(const char *path, int argc, const char *const *argv,
                    char *stdout_buf, u64 stdout_cap, u64 *stdout_len,
                    u64 *exit_code) {
    return lxc_exec_internal(path, argc, argv, 0,
                             stdout_buf, stdout_cap, stdout_len, exit_code);
}

/* VSCode Phase 5: 异步执行——daemon fork+setsid+detach 后立即回
 * EXEC_EXIT(0)，不捕获输出、不等待退出。用于 X server / VSCode 等长
 * 生命周期 GUI 进程。返回 0 = spawn 成功。 */
static int lxc_exec_async(const char *path, int argc,
                          const char *const *argv) {
    u64 exit_code = 1;
    int rc = lxc_exec_internal(path, argc, argv, UTSM_EXEC_FLAG_ASYNC,
                               0, 0, 0, &exit_code);
    if (rc != 0) return rc;
    if (exit_code != 0) {
        log_hex64("[LNXC] exec_async: daemon spawn failed, code=", exit_code);
        return -5;
    }
    return 0;
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
        panic_full("LNX-E11 FILE ENQUEUE FAILED", "ipc_shm_send rejected the file request", 0);
        return -2;
    }

    for (int slice = 0; slice < LXC_MAX_SLICES; slice++) {
        int r = linux_resume();
        if (r != 0) {
            log_hex64("[LNXC] file: linux_resume failed=", (u64)(i64)r);
            return -3;
        }
        for (int i = 0; i < 256; i++) {
            struct utsm_ipc_msg msg;
            if (ipc_shm_recv(&msg) != 0) break;
            if (msg.type == UTSM_MSG_FILE_RESPONSE &&
                msg.data_len >= sizeof(struct ipc_file_response)) {
                my_memcpy(resp, msg.data, sizeof(*resp));
                return 0;
            }
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

/* ===== VSCode Phase 2: 输入转发（PS/2 → virtio-input） ===== */

/* 上次鼠标按钮状态，用于边沿检测（press/release）。 */
static u8 g_prev_mouse_buttons = 0;

static void lxc_input_forward_keyboard(u16 code, u32 value) {
    virtio_input_push_keyboard(code, value);
}

static void lxc_input_forward_mouse(i32 dx, i32 dy, u8 buttons) {
    /* 相对位移 */
    if (dx != 0) virtio_input_push_mouse_rel(VIO_INPUT_REL_X, dx);
    if (dy != 0) virtio_input_push_mouse_rel(VIO_INPUT_REL_Y, dy);

    /* 按钮变化检测：XOR 找出变化的位 */
    u8 changed = buttons ^ g_prev_mouse_buttons;
    if (changed & 0x01)
        virtio_input_push_mouse_button(VIO_INPUT_BTN_LEFT,   (buttons & 0x01) ? 1 : 0);
    if (changed & 0x02)
        virtio_input_push_mouse_button(VIO_INPUT_BTN_RIGHT,  (buttons & 0x02) ? 1 : 0);
    if (changed & 0x04)
        virtio_input_push_mouse_button(VIO_INPUT_BTN_MIDDLE, (buttons & 0x04) ? 1 : 0);
    g_prev_mouse_buttons = buttons;

    /* SYN_REPORT：libinput 依赖此帧结束标记 */
    virtio_input_push_mouse_syn();
}

/* ===== VSCode Phase 3: virtio-gpu scanout 查询转发 =====
 * desktop 在 IDE attached 时周期调用，把 guest 合成的帧 blit 到 framebuffer。
 * 我们只做字段拷贝（linux_compat_scanout_info 与 virtio_gpu_scanout_info 布局一致）。 */
static int lxc_gpu_get_scanout_info(struct linux_compat_scanout_info *out) {
    if (!out) return -1;
    struct virtio_gpu_scanout_info vi;
    int rc = virtio_gpu_get_scanout_info(&vi);
    if (rc != 0) {
        out->host_vaddr = 0;
        out->width = 0; out->height = 0; out->stride = 0;
        out->dirty = 0; out->enabled = 0;
        out->dirty_x = 0; out->dirty_y = 0;
        out->dirty_w = 0; out->dirty_h = 0;
        out->cursor_visible = 0; out->cursor_x = 0; out->cursor_y = 0;
        out->cursor_hot_x = 0; out->cursor_hot_y = 0;
        out->cursor_w = 0; out->cursor_h = 0; out->cursor_bitmap = 0;
        return rc;
    }
    out->host_vaddr = vi.host_vaddr;
    out->width = vi.width;
    out->height = vi.height;
    out->stride = vi.stride;
    out->dirty = vi.dirty;
    out->enabled = vi.enabled;
    out->dirty_x = vi.dirty_x;
    out->dirty_y = vi.dirty_y;
    out->dirty_w = vi.dirty_w;
    out->dirty_h = vi.dirty_h;
    out->cursor_visible = vi.cursor_visible;
    out->cursor_x = vi.cursor_x;
    out->cursor_y = vi.cursor_y;
    out->cursor_hot_x = vi.cursor_hot_x;
    out->cursor_hot_y = vi.cursor_hot_y;
    out->cursor_w = vi.cursor_w;
    out->cursor_h = vi.cursor_h;
    out->cursor_bitmap = vi.cursor_bitmap;
    return 0;
}

static int lxc_run_slice(void) {
    if (!lxc_is_available()) return -1;
    return linux_resume();
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
    lxc_input_forward_keyboard,
    lxc_input_forward_mouse,
    lxc_gpu_get_scanout_info,
    lxc_exec_async,
    lxc_run_slice,
};

const linux_compat_service *linux_compat_get_service(void) {
    return &g_lxc_service;
}

/* ===== Phase 2: SYSTEM/lib .so 全量推送到 Linux guest =====
 *
 * 启动时把 FAT32 根目录下 lib/ 子目录（g_compat_lib_path）中所有 .so
 * 文件流式推送到 Linux guest 的 g_compat_linux_guest_path 目录。
 * 流式推送：按簇（4KB）读取 FAT32 文件 → 立即 lxc_file_write 写入 guest，
 * 不依赖 fat32_io.h 的整文件 256KB 缓冲，可处理 MB 级 .so。
 *
 * 路径假设：build.ps1 把 SYSTEM/ 打包为 FAT32 根，因此 Windows
 * D:\Code\Deshab\SYSTEM\lib\ 在 FAT32 上对应根目录下的 "lib/" 子目录。
 */

/* 把内存中已有数据按 pool 容量分块推送到 Linux 文件。
 * 用于整文件内存数据的批量上传。 */
__attribute__((unused))
static int lxc_file_push_mem(const char *linux_path, const u8 *data, u32 size) {
    if (!lxc_is_available()) return -1;
    if (!linux_path || (!data && size > 0)) return -1;

    u32 pool_cap = ipc_shm_payload_capacity();
    u64 off = 0;
    for (;;) {
        u64 remain = (u64)size - off;
        u64 chunk = remain > pool_cap ? pool_cap : remain;
        u64 written = 0;
        int rc = lxc_file_write(linux_path, off, (const char *)data + off,
                                chunk, &written);
        if (rc != 0) return rc;
        if (written != chunk) return -5;  /* 短写 */
        off += written;
        if (off >= size) break;
    }
    return 0;
}

/* 流式推送：按簇读取 FAT32 文件，立即写入 Linux guest。
 *   first_clus  : FAT32 文件首簇号
 *   file_size   : 文件字节大小
 *   linux_path  : guest 端目标绝对路径
 * 返回 0 成功，负值失败。
 * 复用 lxc_f32_ensure() 已初始化的 FAT32 库（f32_blk_read）。 */
static int lxc_push_fat32_file_streaming(u32 first_clus, u32 file_size,
                                          const char *linux_path) {
    if (!lxc_is_available()) return -1;
    if (!linux_path) return -1;
    if (f32_disk_load() != 0) return -2;

    const f32_bpb *bpb = (const f32_bpb *)f32_disk;
    u32 data_lba = bpb->rsvd + (u32)bpb->fc * bpb->spf;
    u32 fat_byte_off = bpb->rsvd * 512;
    u32 spc = bpb->spc;
    u32 pool_cap = ipc_shm_payload_capacity();
    u32 cluster_bytes = spc * 512;
    if (cluster_bytes > sizeof(f32_cluster)) cluster_bytes = sizeof(f32_cluster);

    /* 用 f32_data 作为单簇中转缓冲（每次最多 pool_cap 字节） */
    u32 buf_cap = pool_cap;
    if (buf_cap > sizeof(f32_data)) buf_cap = sizeof(f32_data);

    u64 off = 0;
    u32 fc = first_clus;
    u32 buf_used = 0;
    while (fc >= 2 && fc < 0x0FFFFFF8 && off < file_size) {
        u32 fc_lba = data_lba + (fc - 2) * spc;
        u32 fc_bytes = spc * 512;
        if (fc_bytes > sizeof(f32_cluster)) fc_bytes = sizeof(f32_cluster);
        if (f32_read_sectors(fc_lba, spc, f32_cluster) != 0) return -3;

        /* 把当前簇数据按 buf_cap 分批写入 */
        u32 ci = 0;
        while (ci < fc_bytes && off + ci < file_size) {
            u32 space = buf_cap - buf_used;
            u32 copy = fc_bytes - ci;
            if (copy > space) copy = space;
            if (copy > file_size - (u32)(off + ci)) copy = (u32)(file_size - (off + ci));
            for (u32 b = 0; b < copy; b++) {
                f32_data[buf_used + b] = f32_cluster[ci + b];
            }
            buf_used += copy;
            ci += copy;
            /* 缓冲满 → 写入 guest */
            if (buf_used >= buf_cap || off + ci >= file_size) {
                u64 written = 0;
                int rc = lxc_file_write(linux_path, off, (const char *)f32_data,
                                        buf_used, &written);
                if (rc != 0) return rc;
                if (written != buf_used) return -5;
                off += written;
                buf_used = 0;
            }
        }
        /* 跟随 FAT 链 */
        u32 fo = fat_byte_off + fc * 4;
        if (fo + 4 > sizeof(f32_disk)) break;
        fc = f32_r32(f32_disk + fo) & 0x0FFFFFFF;
    }
    /* 处理剩余缓冲 */
    if (buf_used > 0 && off < file_size) {
        u64 written = 0;
        int rc = lxc_file_write(linux_path, off, (const char *)f32_data,
                                buf_used, &written);
        if (rc != 0) return rc;
        off += written;
    }
    return (off >= file_size) ? 0 : -6;
}

/* .so 文件收集回调上下文 */
typedef struct {
    u32 count;       /* 已收集的 .so 数量 */
    u32 max_count;   /* 上限 */
    int  error;      /* 0=正常, 非 0=出错 */
} lxc_so_collect_ctx;

/* f32_list_dir 回调：过滤 .so 后缀的文件，记录发现 */
static int lxc_so_collect_cb(const f32_entry *e, void *user_data) {
    lxc_so_collect_ctx *ctx = (lxc_so_collect_ctx *)user_data;
    if (!e || !ctx) return 0;
    if (e->is_dir) return 0;  /* 跳过子目录 */

    /* 判断 .so 后缀（大小写不敏感） */
    const char *name = e->has_lfn ? e->long_name : e->name;
    if (!fat32_lfn_ends_with_ci(name, ".so")) return 0;

    if (ctx->count < ctx->max_count) {
        ctx->count++;
    } else {
        ctx->error = 1;  /* 超过上限 */
        return 1;  /* 停止遍历 */
    }
    return 0;
}

/* 单文件推送回调上下文（用于流式推送所有 .so 到 guest） */
typedef struct {
    const char *guest_path;   /* guest 端目录绝对路径 */
    u32 pushed;               /* 已推送文件数 */
    u32 failed;               /* 推送失败文件数 */
} lxc_push_ctx;

/* f32_list_dir 回调：流式推送每个 .so 到 guest */
static int lxc_push_so_cb(const f32_entry *e, void *user_data) {
    lxc_push_ctx *ctx = (lxc_push_ctx *)user_data;
    if (!e || !ctx) return 0;
    if (e->is_dir) return 0;

    const char *name = e->has_lfn ? e->long_name : e->name;
    if (!fat32_lfn_ends_with_ci(name, ".so")) return 0;

    /* 构造 guest 端路径: <guest_path>/<filename> */
    char linux_path[256];
    u32 pl = 0;
    while (ctx->guest_path[pl] && pl + 1 < sizeof(linux_path)) {
        linux_path[pl] = ctx->guest_path[pl];
        pl++;
    }
    if (pl + 1 >= sizeof(linux_path)) {
        ctx->failed++;
        return 0;  /* 路径过长，跳过 */
    }
    if (linux_path[pl - 1] != '/') {
        linux_path[pl++] = '/';
    }
    /* 追加文件名 */
    u32 nl = 0;
    while (name[nl] && pl + 1 < sizeof(linux_path)) {
        linux_path[pl++] = name[nl++];
    }
    linux_path[pl] = 0;

    log_info("[LNXC] pushing .so:");
    log_info(name);
    log_info("  -> ");
    log_info(linux_path);

    int rc = lxc_push_fat32_file_streaming(e->clus, e->size, linux_path);
    if (rc == 0) {
        ctx->pushed++;
        log_hex64("[LNXC] push ok size=", e->size);
    } else {
        ctx->failed++;
        log_warn("[LNXC] push failed");
        log_hex64("[LNXC] push rc=", (u64)(i64)rc);
    }
    return 0;  /* 继续遍历下一个 */
}

/* 同步 FAT32 lib 目录下所有 .so 到 Linux guest。
 * 调用时机：linux_compat_init 成功后。失败不阻断启动。
 *   guest_lib_path : guest 端库目录路径（如 "/usr/lib/deshab"）
 * 返回 0 成功，负值失败。 */
int lxc_sync_lib_dir(const char *guest_lib_path) {
    if (!guest_lib_path) return -1;
    if (!lxc_is_available()) {
        log_warn("[LNXC] sync_lib_dir: service not available");
        panic_full("LNX-E12 SYNC LIB SERVICE UNAVAILABLE", "lib sync invoked while the compat service is unavailable", 0);
        return -1;
    }
    int rc = lxc_f32_ensure();
    if (rc != 0) {
        log_warn("[LNXC] sync_lib_dir: FAT32 not available");
        panic_full("LNX-E13 SYNC LIB FAT32 UNAVAILABLE", "lib sync invoked without a usable fat32 block device", 0);
        return rc;
    }

    /* 定位 lib 目录簇（lib 在 FAT32 根目录下，不是 SYSTEM/lib！） */
    u32 lib_clus = 0;
    if (f32_find_path_dir_lfn(g_compat_lib_path, &lib_clus) != 0) {
        log_warn("[LNXC] sync_lib_dir: lib dir not found:");
        log_warn(g_compat_lib_path);
        panic_full("LNX-E14 LIB DIR NOT FOUND", "fat32 lib directory for the guest is missing on the system disk", 0);
        return -2;  /* 目录不存在（容忍：可能是首次启动未放库） */
    }
    log_info("[LNXC] sync_lib_dir: lib dir found");
    log_hex64("[LNXC] lib_clus=", lib_clus);

    /* 先枚举确认有 .so 文件（用于日志） */
    lxc_so_collect_ctx collect = {0, 256, 0};
    f32_list_dir(lib_clus, lxc_so_collect_cb, &collect);
    if (collect.error) {
        log_warn("[LNXC] sync_lib_dir: too many .so files, capped at 256");
    }
    log_hex64("[LNXC] sync_lib_dir: .so count=", collect.count);
    if (collect.count == 0) {
        log_info("[LNXC] sync_lib_dir: no .so files, nothing to push");
        return 0;
    }

    /* 流式推送每个 .so */
    lxc_push_ctx pctx;
    pctx.guest_path = guest_lib_path;
    pctx.pushed = 0;
    pctx.failed = 0;
    f32_list_dir(lib_clus, lxc_push_so_cb, &pctx);

    log_hex64("[LNXC] sync_lib_dir: pushed=", pctx.pushed);
    log_hex64("[LNXC] sync_lib_dir: failed=", pctx.failed);
    return (pctx.failed == 0) ? 0 : -3;
}

/* ===== VSCode Phase 5: VSCode tarball → guest /opt/vscode 安装 =====
 *
 * tarball 作为 Limine boot module（路径含 "vscode"）随镜像预加载到主机
 * 内存；本函数把它流式推入 guest /tmp/vscode.tar.gz，再在 guest 内解包到
 * /opt/vscode（/opt 位于 extra-rootfs overlay，持久可写，二次启动经
 * 标记探测直接跳过）。 */

/* guest 内文件可执行性探测：同步 exec /bin/test -e <path>。
 * 返回 1 = 存在，0 = 不存在或探测失败。 */
static int lxc_guest_path_exists(const char *path) {
    const char *argv[3];
    argv[0] = "test";
    argv[1] = "-e";
    argv[2] = path;
    u64 exit_code = 1;
    int rc = lxc_exec("/bin/test", 3, argv, 0, 0, 0, &exit_code);
    return (rc == 0 && exit_code == 0) ? 1 : 0;
}

int lxc_vscode_install(void) {
    if (!lxc_is_available()) {
        log_warn("[LNXC] vscode install: service not available");
        panic_full("LNX-E15 VSCODE SERVICE UNAVAILABLE", "vscode install invoked while the compat service is unavailable", 0);
        return -1;
    }
    log_info("[LNXC] vscode: probe /opt/vscode/bin/code");

    /* 1. 已安装标记：overlay 持久化后二次启动直接跳过 */
    if (lxc_guest_path_exists("/opt/vscode/bin/code")) {
        log_info("[LNXC] vscode: already installed, skipping");
        return 0;
    }

    /* 2. 查找 tarball boot module（缺失属正常——未投放则跳过安装） */
    u64 tar_size = 0;
    const u8 *tar = (const u8 *)linux_find_vscode_module(&tar_size);
    if (!tar || tar_size == 0) {
        return -11;
    }

    log_info("[LNXC] vscode: streaming tarball to guest /tmp ...");
    log_hex64("[LNXC] vscode: tarball size=", tar_size);

    /* 3. 流式推送：module 内存 → /tmp/vscode.tar.gz（payload pool 分块） */
    u32 pool_cap = ipc_shm_payload_capacity();
    u64 off = 0;
    while (off < tar_size) {
        u64 chunk = tar_size - off;
        if (chunk > pool_cap) chunk = pool_cap;
        u64 written = 0;
        int rc = lxc_file_write("/tmp/vscode.tar.gz", off,
                                (const char *)tar + off, chunk, &written);
        if (rc != 0 || written != chunk) {
            log_warn("[LNXC] vscode: tarball push failed");
            log_hex64("[LNXC] vscode: push off=", off);
            panic_full("LNX-E16 VSCODE TARBALL PUSH FAILED", "streaming the vscode tarball into the guest failed", 0);
            return -12;
        }
        off += written;
    }
    log_info("[LNXC] vscode: tarball pushed ok");

    /* 4. mkdir -p /opt/vscode */
    {
        const char *argv[3];
        argv[0] = "mkdir";
        argv[1] = "-p";
        argv[2] = "/opt/vscode";
        u64 ec = 1;
        int rc = lxc_exec("/bin/mkdir", 3, argv, 0, 0, 0, &ec);
        if (rc != 0 || ec != 0) {
            log_warn("[LNXC] vscode: mkdir /opt/vscode failed");
            panic_full("LNX-E17 VSCODE MKDIR FAILED", "mkdir -p /opt/vscode failed inside the guest", 0);
            return -13;
        }
    }

    /* 5. 解包：VSCode 官方 tarball 顶层为 VSCode-linux-x64/，
     *    --strip-components=1 把内容直接落入 /opt/vscode；
     *    若 tarball 是平铺结构（无顶层目录）则回退不解层重试。 */
    int unpacked = 0;
    {
        const char *argv[7];
        argv[0] = "tar";
        argv[1] = "-xzf";
        argv[2] = "/tmp/vscode.tar.gz";
        argv[3] = "-C";
        argv[4] = "/opt/vscode";
        argv[5] = "--strip-components=1";
        argv[6] = 0;
        u64 ec = 1;
        int rc = lxc_exec("/bin/tar", 6, argv, 0, 0, 0, &ec);
        if (rc == 0 && ec == 0) unpacked = 1;
    }
    if (!unpacked) {
        log_warn("[LNXC] vscode: strip-components extract failed, trying flat");
        const char *argv[5];
        argv[0] = "tar";
        argv[1] = "-xzf";
        argv[2] = "/tmp/vscode.tar.gz";
        argv[3] = "-C";
        argv[4] = "/opt/vscode";
        u64 ec = 1;
        int rc = lxc_exec("/bin/tar", 5, argv, 0, 0, 0, &ec);
        if (rc == 0 && ec == 0) unpacked = 1;
    }

    /* 6. 清理 guest 端 tarball（成败都清，/tmp 是 tmpfs 占 guest 内存） */
    {
        const char *argv[3];
        argv[0] = "rm";
        argv[1] = "-f";
        argv[2] = "/tmp/vscode.tar.gz";
        u64 ec = 0;
        lxc_exec("/bin/rm", 3, argv, 0, 0, 0, &ec);
    }

    if (!unpacked) {
        log_warn("[LNXC] vscode: extract failed");
        panic_full("LNX-E18 VSCODE EXTRACT FAILED", "tar extraction of the vscode tarball failed in the guest", 0);
        return -13;
    }

    /* 7. 验证：/opt/vscode/bin/code 存在即可启动 */
    if (!lxc_guest_path_exists("/opt/vscode/bin/code")) {
        log_warn("[LNXC] vscode: verify failed (bin/code missing)");
        panic_full("LNX-E19 VSCODE VERIFY FAILED", "/opt/vscode/bin/code missing after the install", 0);
        return -14;
    }

    log_info("[LNXC] vscode: install ok (/opt/vscode/bin/code)");
    return 0;
}
