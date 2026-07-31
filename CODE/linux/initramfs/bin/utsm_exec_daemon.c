// SPDX-License-Identifier: GPL-2.0
/*
 * utsm_exec_daemon — Linux guest side exec daemon for Deshab Linux compat layer.
 *
 * Runs as the first userspace process (PID 2, after init) in the Linux guest.
 * Communicates with UTSM via /dev/utsm ioctl interface:
 *
 *   1. Send EXEC_READY, then ioctl(PARK) → HLT → guest parks
 *      (this is the first HLT; linux_launch() returns to UTSM)
 *   2. On vmresume: ioctl(RECV_MSG) → EXEC_REQUEST
 *   3. fork + exec the requested program, capture stdout/stderr via pipe
 *   4. Stream output back via ioctl(SEND_MSG, EXEC_STDOUT/STDERR)
 *   5. Send EXEC_EXIT with waitpid exit code
 *   6. Loop: ioctl(PARK) → wait for next request
 *
 * Build: static musl/glibc binary, stripped, copied into initramfs /bin/.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Shared IPC protocol — must match UTSM side */
#include "../../../utsm-ipc/ipc_proto.h"

/* Ioctl definitions — must match utsm_hcall.c driver */
#define UTSM_IOCTL_RECV_MSG  _IOWR('U', 1, struct utsm_ioctl_msg)
#define UTSM_IOCTL_SEND_MSG  _IOWR('U', 2, struct utsm_ioctl_msg)
#define UTSM_IOCTL_PARK      _IO('U', 3)
#define UTSM_IOCTL_GET_READY _IOR('U', 4, int)
#define UTSM_IOCTL_WRITE_POOL _IOW('U', 5, struct utsm_ioctl_pool)
#define UTSM_IOCTL_READ_POOL  _IOWR('U', 6, struct utsm_ioctl_pool)

struct utsm_ioctl_msg {
    unsigned int type;
    unsigned int data_len;
    unsigned int reserved;
    unsigned int buf_size;
    unsigned char data[UTSM_IPC_MSG_DATA_SIZE];
};

/* Pool transfer descriptor — must match utsm_hcall.c driver */
struct utsm_ioctl_pool {
    unsigned int offset;
    unsigned int len;
    unsigned long long user_buf;
};

static int g_utsm_fd = -1;

/* Send a typed message to UTSM via the linux_to_utsm ring */
static int send_msg(unsigned int type, const void *data, unsigned int len)
{
    struct utsm_ioctl_msg umsg;
    memset(&umsg, 0, sizeof(umsg));
    umsg.type = type;
    umsg.data_len = len;
    umsg.buf_size = UTSM_IPC_MSG_DATA_SIZE;
    if (data && len > 0) {
        if (len > UTSM_IPC_MSG_DATA_SIZE)
            len = UTSM_IPC_MSG_DATA_SIZE;
        memcpy(umsg.data, data, len);
        umsg.data_len = len;
    }
    return ioctl(g_utsm_fd, UTSM_IOCTL_SEND_MSG, &umsg);
}

/* Receive a message from UTSM (non-blocking). Returns 0 on success, -EAGAIN if empty. */
static int recv_msg(struct utsm_ioctl_msg *umsg)
{
    memset(umsg, 0, sizeof(*umsg));
    umsg->buf_size = UTSM_IPC_MSG_DATA_SIZE;
    return ioctl(g_utsm_fd, UTSM_IOCTL_RECV_MSG, umsg);
}

/* Park: HLT → VM-Exit → UTSM parks guest. Returns on vmresume. */
static int park(void)
{
    return ioctl(g_utsm_fd, UTSM_IOCTL_PARK);
}

/* Best-effort write to the child's stderr (exec failure path);
 * the result is intentionally ignored. */
static void err_write(const char *s, unsigned int len)
{
    ssize_t n = write(STDERR_FILENO, s, len);
    (void)n;
}

/* ===== Exec request handler =====
 *
 * Fork a child to exec the requested program. Parent reads stdout/stderr
 * from a pipe and streams chunks back to UTSM via EXEC_STDOUT/EXEC_STDERR
 * messages. Finally sends EXEC_EXIT with the child's exit status.
 *
 * argv parsing: argv_blob contains NUL-terminated argv[0..argc-1]
 * concatenated. We split it into a char* array for execv. */
static void handle_exec_request(const struct ipc_exec_request *req)
{
    /* Parse argv_blob into argv[] array */
    char *argv[UTSM_EXEC_MAX_ARGS + 1];
    int argc = (int)req->argc;
    if (argc > UTSM_EXEC_MAX_ARGS) argc = UTSM_EXEC_MAX_ARGS;
    if (argc < 1) {
        struct ipc_exec_exit ex = { .exit_code = 1, .reserved = 0 };
        send_msg(UTSM_MSG_EXEC_EXIT, &ex, sizeof(ex));
        return;
    }

    const char *blob = req->argv_blob;
    int blob_consumed = 0;
    for (int i = 0; i < argc; i++) {
        if (blob_consumed >= UTSM_EXEC_ARGV_MAX) {
            argv[i] = (char *)"";
            continue;
        }
        argv[i] = (char *)(blob + blob_consumed);
        int slen = strnlen(blob + blob_consumed, UTSM_EXEC_ARGV_MAX - blob_consumed);
        blob_consumed += slen + 1;
    }
    argv[argc] = NULL;

    /* Create a pipe for child stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        struct ipc_exec_exit ex = { .exit_code = 126, .reserved = 0 };
        send_msg(UTSM_MSG_EXEC_EXIT, &ex, sizeof(ex));
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        /* fork failed */
        close(pipefd[0]);
        close(pipefd[1]);
        struct ipc_exec_exit ex = { .exit_code = 127, .reserved = 0 };
        send_msg(UTSM_MSG_EXEC_EXIT, &ex, sizeof(ex));
        return;
    }

    if (pid == 0) {
        /* Child: redirect stdout + stderr to pipe write end, then exec */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execv(req->path, argv);
        /* execv only returns on failure */
        const char *err1 = "utsm_exec_daemon: exec failed: ";
        err_write(err1, (unsigned int)strlen(err1));
        err_write(req->path, (unsigned int)strnlen(req->path, UTSM_EXEC_PATH_MAX));
        const char *err2 = ": ";
        err_write(err2, (unsigned int)strlen(err2));
        const char *emsg = strerror(errno);
        err_write(emsg, (unsigned int)strlen(emsg));
        err_write("\n", 1);
        _exit(127);
    }

    /* Parent: read child output from pipe, stream to UTSM */
    close(pipefd[1]);

    char buf[UTSM_IPC_MSG_DATA_SIZE];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        send_msg(UTSM_MSG_EXEC_STDOUT, buf, (unsigned int)n);
    }
    close(pipefd[0]);

    /* Wait for child and send exit code */
    int status = 0;
    waitpid(pid, &status, 0);

    struct ipc_exec_exit ex;
    ex.reserved = 0;
    if (WIFEXITED(status)) {
        ex.exit_code = (unsigned int)WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ex.exit_code = 128u + (unsigned int)WTERMSIG(status);
    } else {
        ex.exit_code = 1;
    }
    send_msg(UTSM_MSG_EXEC_EXIT, &ex, sizeof(ex));
}

/* ===== File transfer handlers (Phase 3) =====
 *
 * UTSM sends FILE_LIST_REQUEST / FILE_READ_REQUEST with an ipc_file_request
 * payload. Bulk result data is staged into the shared payload_pool via
 * UTSM_IOCTL_WRITE_POOL, then a FILE_RESPONSE message (ipc_file_response)
 * carries status + data_len + total_size back to UTSM.
 *
 * Directory listing format: one entry per line, directories suffixed '/'. */

/* Staging buffer for file reads / listings (capped by pool capacity ~1MB) */
#define FT_STAGE_SIZE (768 * 1024)
static unsigned char g_ft_stage[FT_STAGE_SIZE];

static int pool_write(unsigned int offset, const void *buf, unsigned int len)
{
    struct utsm_ioctl_pool p;
    p.offset = offset;
    p.len = len;
    p.user_buf = (unsigned long long)(uintptr_t)buf;
    return ioctl(g_utsm_fd, UTSM_IOCTL_WRITE_POOL, &p);
}

static int pool_read(unsigned int offset, void *buf, unsigned int len)
{
    struct utsm_ioctl_pool p;
    p.offset = offset;
    p.len = len;
    p.user_buf = (unsigned long long)(uintptr_t)buf;
    return ioctl(g_utsm_fd, UTSM_IOCTL_READ_POOL, &p);
}

static unsigned int errno_to_file_status(int e)
{
    switch (e) {
    case ENOENT:  return UTSM_FILE_ERR_NOENT;
    case ENOTDIR: return UTSM_FILE_ERR_NOTDIR;
    case EISDIR:  return UTSM_FILE_ERR_ISDIR;
    case EACCES:  return UTSM_FILE_ERR_PERM;
    case ENOSPC:
    case EFBIG:   return UTSM_FILE_ERR_NOSPC;
    default:      return UTSM_FILE_ERR_IO;
    }
}

static void send_file_response(unsigned int status, unsigned int data_len,
                               unsigned long long total_size)
{
    struct ipc_file_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    resp.data_len = data_len;
    resp.total_size = total_size;
    send_msg(UTSM_MSG_FILE_RESPONSE, &resp, sizeof(resp));
}

/* FILE_LIST_REQUEST: list directory entries as "name\n" ("name/\n" for dirs) */
static void handle_file_list(const struct ipc_file_request *req)
{
    char path[UTSM_FILE_PATH_MAX + 1];
    memcpy(path, req->path, UTSM_FILE_PATH_MAX);
    path[UTSM_FILE_PATH_MAX] = '\0';

    if (req->pool_capacity == 0 || req->pool_offset >= UTSM_IPC_PAYLOAD_SIZE) {
        send_file_response(UTSM_FILE_ERR_INVAL, 0, 0);
        return;
    }

    DIR *d = opendir(path);
    if (!d) {
        send_file_response(errno_to_file_status(errno), 0, 0);
        return;
    }

    unsigned int cap = req->pool_capacity;
    if (cap > FT_STAGE_SIZE) cap = FT_STAGE_SIZE;

    unsigned int used = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        /* Determine directory flag: d_type if available, else stat */
        int is_dir = 0;
        if (de->d_type == DT_DIR) {
            is_dir = 1;
        } else if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
            struct stat st;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
                is_dir = 1;
        }
        int nl = snprintf((char *)g_ft_stage + used, cap - used,
                          "%s%s\n", de->d_name, is_dir ? "/" : "");
        if (nl < 0) break;
        if ((unsigned int)nl >= cap - used) {
            /* 缓冲已满：本条未完整写入，截断结束 */
            used = cap;
            break;
        }
        used += (unsigned int)nl;
    }
    closedir(d);

    if (used > 0 && pool_write(req->pool_offset, g_ft_stage, used) != 0) {
        send_file_response(UTSM_FILE_ERR_IO, 0, 0);
        return;
    }
    send_file_response(UTSM_FILE_OK, used, 0);
}

/* FILE_READ_REQUEST: read up to pool_capacity bytes at file_offset */
static void handle_file_read(const struct ipc_file_request *req)
{
    char path[UTSM_FILE_PATH_MAX + 1];
    memcpy(path, req->path, UTSM_FILE_PATH_MAX);
    path[UTSM_FILE_PATH_MAX] = '\0';

    if (req->pool_capacity == 0 || req->pool_offset >= UTSM_IPC_PAYLOAD_SIZE) {
        send_file_response(UTSM_FILE_ERR_INVAL, 0, 0);
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        send_file_response(errno_to_file_status(errno), 0, 0);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        send_file_response(UTSM_FILE_ERR_ISDIR, 0, (unsigned long long)st.st_size);
        return;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_file_response(errno_to_file_status(errno), 0,
                           (unsigned long long)st.st_size);
        return;
    }

    unsigned int cap = req->pool_capacity;
    if (cap > FT_STAGE_SIZE) cap = FT_STAGE_SIZE;

    off_t off = (off_t)req->file_offset;
    ssize_t n = pread(fd, g_ft_stage, cap, off);
    int saved = errno;
    close(fd);

    if (n < 0) {
        send_file_response(errno_to_file_status(saved), 0,
                           (unsigned long long)st.st_size);
        return;
    }

    if (n > 0 && pool_write(req->pool_offset, g_ft_stage, (unsigned int)n) != 0) {
        send_file_response(UTSM_FILE_ERR_IO, 0, (unsigned long long)st.st_size);
        return;
    }
    send_file_response(UTSM_FILE_OK, (unsigned int)n,
                       (unsigned long long)st.st_size);
}

/* FILE_WRITE_REQUEST: write payload_pool data to file at file_offset.
 * UTSM pre-stages req->pool_capacity bytes at req->pool_offset in the pool.
 * Convention: file_offset == 0 truncates the file first (push = replace);
 * the file is created with mode 0644 if missing. Data is moved in
 * FT_STAGE_SIZE pieces so a full pool (~1MB) can be consumed per request. */
static void handle_file_write(const struct ipc_file_request *req)
{
    char path[UTSM_FILE_PATH_MAX + 1];
    memcpy(path, req->path, UTSM_FILE_PATH_MAX);
    path[UTSM_FILE_PATH_MAX] = '\0';

    if (req->pool_offset >= UTSM_IPC_PAYLOAD_SIZE ||
        req->pool_capacity > UTSM_IPC_PAYLOAD_SIZE - req->pool_offset) {
        send_file_response(UTSM_FILE_ERR_INVAL, 0, 0);
        return;
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        send_file_response(UTSM_FILE_ERR_ISDIR, 0, 0);
        return;
    }

    int flags = O_WRONLY | O_CREAT;
    if (req->file_offset == 0)
        flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        send_file_response(errno_to_file_status(errno), 0, 0);
        return;
    }

    unsigned int remaining = req->pool_capacity;
    unsigned int done = 0;
    while (remaining > 0) {
        unsigned int chunk = remaining;
        if (chunk > FT_STAGE_SIZE) chunk = FT_STAGE_SIZE;

        if (pool_read(req->pool_offset + done, g_ft_stage, chunk) != 0) {
            close(fd);
            send_file_response(UTSM_FILE_ERR_IO, 0, 0);
            return;
        }
        ssize_t n = pwrite(fd, g_ft_stage, chunk,
                           (off_t)(req->file_offset + done));
        if (n < 0) {
            int saved = errno;
            close(fd);
            send_file_response(errno_to_file_status(saved), done, 0);
            return;
        }
        done += (unsigned int)n;
        remaining -= (unsigned int)n;
        if ((unsigned int)n < chunk)
            break;  /* short write (e.g. ENOSPC on next call would fail) */
    }

    unsigned long long total = 0;
    if (fstat(fd, &st) == 0)
        total = (unsigned long long)st.st_size;
    close(fd);
    send_file_response(UTSM_FILE_OK, done, total);
}

/* ===== Main loop ===== */

int main(void)
{
    g_utsm_fd = open("/dev/utsm", O_RDWR);
    if (g_utsm_fd < 0) {
        /* Cannot communicate with UTSM — exit */
        return 1;
    }

    /* Announce daemon ready. UTSM's linux_launch() will see the subsequent
     * HLT (via PARK ioctl) and return, marking the compat layer ready. */
    send_msg(UTSM_MSG_EXEC_READY, NULL, 0);

    /* First park: this is the HLT that causes linux_launch() to return. */
    park();

    /* Main loop: process exec requests */
    for (;;) {
        struct utsm_ioctl_msg umsg;

        /* Drain any pending messages (there may be multiple if UTSM
         * queued several before vmresume). */
        while (recv_msg(&umsg) == 0) {
            if (umsg.type == UTSM_MSG_EXEC_REQUEST &&
                umsg.data_len >= sizeof(struct ipc_exec_request)) {
                handle_exec_request((const struct ipc_exec_request *)umsg.data);
            } else if (umsg.type == UTSM_MSG_FILE_LIST_REQUEST &&
                       umsg.data_len >= sizeof(struct ipc_file_request)) {
                handle_file_list((const struct ipc_file_request *)umsg.data);
            } else if (umsg.type == UTSM_MSG_FILE_READ_REQUEST &&
                       umsg.data_len >= sizeof(struct ipc_file_request)) {
                handle_file_read((const struct ipc_file_request *)umsg.data);
            } else if (umsg.type == UTSM_MSG_FILE_WRITE_REQUEST &&
                       umsg.data_len >= sizeof(struct ipc_file_request)) {
                handle_file_write((const struct ipc_file_request *)umsg.data);
            }
            /* Ignore other message types (HELLO, PING, etc.) */
        }

        /* Responses (if any) are already queued. Park to let UTSM read
         * them and return to the shell; a spurious wakeup with no
         * request simply parks again. */
        park();
    }

    close(g_utsm_fd);
    return 0;
}
