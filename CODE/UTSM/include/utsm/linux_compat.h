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

    /* 列出 Linux 目录内容（Phase 3 文件传输）。
     *   path    : Linux 绝对路径（如 "/usr/bin"）
     *   buf/cap : 接收列表文本的缓冲区（每行一项，目录名带 '/' 后缀）
     *   out_len : 实际写入字节数（可为 NULL）
     * 返回 0 成功，负值失败（-1=服务不可用, -2=IPC 发送失败,
     *         -3=linux_resume 失败, -4=无响应, -5=Linux 侧错误,
     *         -6=缓冲区不足被截断，此时 out_len 仍给出截断长度）。 */
    int (*file_list)(const char *path, char *buf, u64 cap, u64 *out_len);

    /* 读取 Linux 文件内容（Phase 3 文件传输，支持大文件分块）。
     *   path      : Linux 绝对路径（如 "/etc/hostname"）
     *   offset    : 文件内起始偏移（首次读传 0）
     *   buf/cap   : 接收文件数据的缓冲区
     *   out_len   : 本次实际读取字节数（可为 NULL）
     *   out_total : 文件总大小（可为 NULL）
     * 返回 0 成功（含 EOF：out_len=0 且 offset>=total），负值失败
     *        （错误码同 file_list）。 */
    int (*file_read)(const char *path, u64 offset, char *buf, u64 cap,
                     u64 *out_len, u64 *out_total);

    /* 查询 Linux guest 状态字符串（调试用），写入 buf，返回字符串长度。 */
    int (*status)(char *buf, u64 cap);

    /* 写入 Linux 文件（Phase 3 文件传输，支持大文件分块）。
     * 调用方以递增 offset 循环调用即可上传任意大小文件。
     *   path        : Linux 绝对路径（不存在则以 0644 创建；
     *                 offset==0 时先截断，即 push 语义为整文件替换）
     *   offset      : 文件内写入偏移（首次写传 0）
     *   buf/len     : 待写数据（len 会被钳制到 payload pool 容量，
     *                 实际写入量以 out_written 为准；len=0 仅创建/截断）
     *   out_written : 本次实际写入字节数（可为 NULL）
     * 返回 0 成功，负值失败（错误码同 file_list）。 */
    int (*file_write)(const char *path, u64 offset, const char *buf, u64 len,
                      u64 *out_written);

    /* 跨内核文件拉取：Linux 文件 → UTSM FAT32 系统盘根目录（≤256KB，整文件）。
     *   linux_path : Linux 绝对路径
     *   fat32_name : UTSM FAT32 根目录文件名（如 "HOSTNAME.TXT"，8.3 可转换）
     *   out_size   : 实际传输字节数（可为 NULL）
     * 返回 0 成功，负值失败（-7=文件超过 256KB 暂存上限, -9=无块设备,
     *         -10=非法 FAT32 名, -12=FAT32 写失败，其余同 file_list）。 */
    int (*file_pull)(const char *linux_path, const char *fat32_name,
                     u64 *out_size);

    /* 跨内核文件推送：UTSM FAT32 系统盘根目录 → Linux 文件（≤256KB，整文件替换）。
     *   fat32_name : UTSM FAT32 根目录文件名（8.3 可转换）
     *   linux_path : Linux 绝对路径
     *   out_size   : 实际传输字节数（可为 NULL）
     * 返回 0 成功，负值失败（-11=文件不存在，其余同 file_pull）。 */
    int (*file_push)(const char *fat32_name, const char *linux_path,
                     u64 *out_size);
} linux_compat_service;

/* ===== UTSM 内部接口（不进服务表，供 hypercall.c 等内核代码使用） =====
 *
 * UTSM FAT32 系统盘根目录文件访问（经 block_get_api() + fat32_io）。
 * name 为普通文件名（内部做 8.3 转换）。
 * 返回 0 成功；负值：-9 无块设备, -10 非法文件名, -11 文件不存在,
 * -12 磁盘 I/O 错误。 */
int lxc_f32_read_file(const char *name, u8 **out_data, u32 *out_size);
int lxc_f32_write_file(const char *name, const u8 *data, u32 size);

/* 获取全局 Linux 兼容层服务实例指针（UTSM 内部使用，
 * 外部通过 boot context reserved[5] 获取）。 */
const linux_compat_service *linux_compat_get_service(void);

/* 初始化 Linux 兼容层服务（在 linux_launch 成功后调用）。 */
void linux_compat_init(void);

#endif /* UTSM_LINUX_COMPAT_H */
