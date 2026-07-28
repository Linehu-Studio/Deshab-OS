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
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/types.h>

/* Shared IPC protocol — must match UTSM side */
#include "../../../utsm-ipc/ipc_proto.h"

/* Ioctl definitions — must match utsm_hcall.c driver */
#define UTSM_IOCTL_RECV_MSG  _IOWR('U', 1, struct utsm_ioctl_msg)
#define UTSM_IOCTL_SEND_MSG  _IOWR('U', 2, struct utsm_ioctl_msg)
#define UTSM_IOCTL_PARK      _IO('U', 3)
#define UTSM_IOCTL_GET_READY _IOR('U', 4, int)

struct utsm_ioctl_msg {
    unsigned int type;
    unsigned int data_len;
    unsigned int reserved;
    unsigned int buf_size;
    unsigned char data[UTSM_IPC_MSG_DATA_SIZE];
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
        write(STDERR_FILENO, err1, strlen(err1));
        write(STDERR_FILENO, req->path, strnlen(req->path, UTSM_EXEC_PATH_MAX));
        const char *err2 = ": ";
        write(STDERR_FILENO, err2, strlen(err2));
        const char *emsg = strerror(errno);
        write(STDERR_FILENO, emsg, strlen(emsg));
        write(STDERR_FILENO, "\n", 1);
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
        int got_request = 0;
        while (recv_msg(&umsg) == 0) {
            if (umsg.type == UTSM_MSG_EXEC_REQUEST &&
                umsg.data_len >= sizeof(struct ipc_exec_request)) {
                handle_exec_request((const struct ipc_exec_request *)umsg.data);
                got_request = 1;
            }
            /* Ignore other message types (HELLO, PING, etc.) */
        }

        /* If we processed a request, the response is already sent.
         * Park to let UTSM read the response and return to shell.
         * If no request (spurious wakeup), just park again. */
        park();
    }

    close(g_utsm_fd);
    return 0;
}
