#ifndef UTSM_LINUX_COMPAT_H
#define UTSM_LINUX_COMPAT_H

/* linux_compat.h — Deshab Linux 程序兼容层服务接口。
 *
 * 依赖 UTSM+Linux 双内核架构：Linux guest 作为 daemon 在 VMX guest 中
 * park-and-resume 驻留。UTSM 侧的 linux_compat_service 通过 IPC 共享内存
 * 向 Linux daemon 发送 exec 请求，并 vmresume 唤醒 guest 执行。
 *
 * 执行流程：
 *   1. shell.elf / cmd.elf 通过 boot context 获取 linux_compat_service 指针
 *   2. 调用 exec(path, argc, argv, ...) 写 IPC 请求到 utsm_to_linux ring
 *   3. exec() 调用 linux_resume() → vmresume → Linux daemon fork+exec
 *   4. daemon 通过 linux_to_utsm ring 回传 stdout/stderr/exit，然后 HLT park
 *   5. exec() 从 ring 读取输出，返回给调用方
 *
 * 服务指针通过 dsk_boot_context.reserved[5] 暴露给 DSK 用户态（shell/cmd/desktop）。
 */

#include <utsm/types.h>

#define LINUX_COMPAT_MAGIC 0x4C4E58434F4D5000ULL  /* "LNXCOMP\0" */

/* Linux 兼容层服务表 — 通过 boot context reserved[5] 传递 */
typedef struct linux_compat_service {
    u64 magic;        /* LINUX_COMPAT_MAGIC */

    /* 查询 Linux 兼容层是否可用（Linux guest 已 park 且 IPC 就绪）。
     * 返回 1 可用，0 不可用。 */
    int (*is_available)(void);

    /* 执行一个 Linux 程序。
     *   path       : 程序路径（如 "/bin/ls"），NUL-terminated
     *   argc/argv  : 参数向量（argv[0] 通常为程序名）
     *   stdout_buf : 接收 stdout 的缓冲区（可为 NULL，此时丢弃 stdout）
     *   stdout_cap : stdout_buf 容量（字节）
     *   stdout_len : 输出实际写入的字节数（可为 NULL）
     *   exit_code  : 输出进程退出码（可为 NULL）
     * 返回 0 成功，负值失败（-1=服务不可用, -2=IPC 发送失败,
     *         -3=linux_resume 失败, -4=超时无响应）。 */
    int (*exec)(const char *path, int argc, const char *const *argv,
                char *stdout_buf, u64 stdout_cap, u64 *stdout_len,
                u64 *exit_code);

    /* 查询 Linux guest 状态字符串（调试用），写入 buf，返回字符串长度。 */
    int (*status)(char *buf, u64 cap);
} linux_compat_service;

/* 获取全局 Linux 兼容层服务实例指针（UTSM 内部使用，
 * 外部通过 boot context reserved[5] 获取）。 */
const linux_compat_service *linux_compat_get_service(void);

/* 初始化 Linux 兼容层服务（在 linux_launch 成功后调用）。 */
void linux_compat_init(void);

#endif /* UTSM_LINUX_COMPAT_H */
