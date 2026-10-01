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

/* VSCode Phase 3: virtio-gpu scanout 查询结果。
 * 镜像 virtio_mmio.h 的 virtio_gpu_scanout_info 字段布局，保持公共 ABI 自洽
 * （不强制消费者 include virtio_mmio.h）。必须在 linux_compat_service 之前
 * 定义，因为服务表中有指向此类型的函数指针。 */
struct linux_compat_scanout_info {
    void *host_vaddr;
    u32 width;
    u32 height;
    u32 stride;
    u32 dirty;
    u32 enabled;
    /* Phase 7: 累积 dirty rect（scanout 坐标系并集，查询随 dirty 清零）。
     * dirty=0 时全为 0；desktop 据此只 blit 变化区域（尾部追加，ABI 兼容）。 */
    u32 dirty_x;
    u32 dirty_y;
    u32 dirty_w;
    u32 dirty_h;
    /* P7.6: hardware cursor state（尾部追加，ABI 兼容）。 */
    u32 cursor_visible;
    u32 cursor_x;
    u32 cursor_y;
    u32 cursor_hot_x;
    u32 cursor_hot_y;
    u32 cursor_w;
    u32 cursor_h;
    void *cursor_bitmap;
};

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

    /* VSCode Phase 2: 输入转发到 Linux guest（virtio-input 键盘+鼠标）。
     * 由 desktop 在 IDE attached 时调用，把 Deshab PS/2 事件注入 guest。
     * 事件排到 pending ring，由 vmexit poll 路径排空到 eventq + 注入 IRQ。
     *
     *   input_forward_keyboard(code, value)
     *     code  : linux keycode（KEY_A=30 等，见 linux/input-event-codes.h）
     *     value : 0=release, 1=press, 2=repeat
     *
     *   input_forward_mouse(dx, dy, buttons)
     *     dx, dy  : 相对位移（像素差，可为负）
     *     buttons : 按钮位图 bit0=左, bit1=右, bit2=中（内部与上次状态比较，
     *               仅变化的按钮产生 press/release 事件；末尾自动追加 SYN） */
    void (*input_forward_keyboard)(u16 code, u32 value);
    void (*input_forward_mouse)(i32 dx, i32 dy, u8 buttons);

    /* VSCode Phase 3: 查询 virtio-gpu scanout 状态（desktop IDE blit 用）。
     * 在 IDE attached 时由 desktop 周期调用：若 dirty=1，把 host_vaddr 指向的
     * scanout 影子缓冲（width×height，stride 字节/行，XRGB8888）blit 到
     * framebuffer 的 IDE host 区域，然后重新查询以清零 dirty。
     *   out->host_vaddr : scanout 影子缓冲 host 虚拟地址（SAS-R0 下可直接解引用）
     *   out->enabled    : 1 = scanout 已绑定 2D resource（X server 已 SET_SCANOUT）
     *   out->dirty      : 1 = 自上次查询有新帧（查询会清零该位）
     * 返回 0 成功（enabled=0 时其余字段为零），-1 后端未就绪（无 surface pool）。 */
    int (*gpu_get_scanout_info)(struct linux_compat_scanout_info *out);

    /* VSCode Phase 5: 异步执行 Linux 程序（daemon fork+setsid+detach）。
     * 与 exec 语义相同，但 daemon 在 spawn 成功后立即回 EXEC_EXIT(0)，
     * 不捕获 stdout、不等待进程退出——用于启动 X server / VSCode 等
     * 长生命周期 GUI 进程而不阻塞 UTSM 侧。子进程 stdio 重定向到
     * /dev/null，环境自带 LD_LIBRARY_PATH=/usr/lib/deshab 与 DISPLAY=:0。
     * 返回 0 = 已成功 spawn（不代表程序后续存活），负值失败
     *        （-1=服务不可用, -2=IPC 发送失败, -3=linux_resume 失败,
     *         -4=无响应, -5=daemon 报告 spawn 失败）。 */
    int (*exec_async)(const char *path, int argc, const char *const *argv);

    /* 不发送 IPC，只跑一段 Linux vCPU（HLT 或 preemption timeslice）。
     * KDE/Plasma 全屏循环用这个唤醒 async 子进程，避免再 exec sleep。
     * 返回 0 成功，负值同 linux_resume。 */
    int (*run_slice)(void);
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

/* 同步 FAT32 lib 目录下所有 .so 文件到 Linux guest。
 *   guest_lib_path : guest 端库目录路径（如 "/usr/lib/deshab"）
 * 调用时机：linux_compat_init 成功后。失败不阻断启动。
 * 返回 0 成功，负值失败。 */
int lxc_sync_lib_dir(const char *guest_lib_path);

/* VSCode Phase 5: 把 VSCode tarball（Limine boot module，路径含 "vscode"）
 * 安装到 Linux guest 的 /opt/vscode（经 overlay 持久化在 extra-rootfs 卷）。
 * 流程：已安装标记探测（/opt/vscode/bin/code 可执行则跳过）→
 *       流式推送 module 字节到 guest /tmp/vscode.tar.gz →
 *       mkdir -p /opt/vscode → tar 解包（--strip-components=1，失败回退不解层）→
 *       清理 /tmp tarball → 验证 /opt/vscode/bin/code。
 * 调用时机：linux_compat_init 成功后（kernel main，FUCK [compat] vscode_install=1）。
 * 返回 0 成功或已安装；负值失败（-1 服务不可用, -11 无 tarball module,
 * -12 推送失败, -13 解包失败, -14 验证失败）。所有失败均不阻断启动。 */
int lxc_vscode_install(void);

#endif /* UTSM_LINUX_COMPAT_H */
