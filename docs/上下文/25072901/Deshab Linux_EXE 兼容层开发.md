# Deshab Linux/EXE 兼容层开发

> **Workspace:** d:\Code\Deshab

---

## 任务目标

为 Deshab 添加完整的 Linux 程序兼容层（依赖双内核架构）和 PE/EXE 兼容层（写在 UTSM 里），编写 cmd.exe 编译到 SYSTEM/system/deshab64，桌面命令行1是 shell.elf（原生+Linux），命令行2是 cmd。

## 架构总览

```
桌面命令行1: shell.elf (原生系统命令行 + linux 命令运行 Linux 程序)
桌面命令行2: cmd.elf  (Windows 风格命令行 + PE/EXE 兼容)
```

## 实施阶段

### Phase 1: PE/EXE 兼容层（UTSM 内置）

**新增文件**（`CODE/UTSM/pe/`）:

| 文件 | 职责 |
|------|------|
| `pe_loader.c/h` | PE32+ 64位原生 Ring0 执行 + PE32 32位 x86 解释器执行 |
| `pe_shim.c/h` | Windows API shim（Kernel32/User32/MSVCRT 子集），使用 `__attribute__((ms_abi))` 处理 MS x64 ABI |
| `x86emu32.c/h` | x86-32 指令解释器（PE32 运行时），支持通用寄存器/标志/FPU/SSE/字符串操作/rep前缀 |
| `pe_service.c/h` | PE 服务组装：统一 load/run/unload 接口，PE32+ 直接执行 + PE32 经解释器执行 |

**PE 服务暴露**：`dsk_loader.c` 通过 `reserved[4]` 传递 `pe_service` 指针给 DSK/cmd.elf。

**PE32+ 加载流程**：解析 COFF header → 段映射到 preferred_base → 应用重定位（IMAGE_REL_BASED_DIR64/HIGHLOW）→ 解析 IAT → 填充 shim 函数 → 跳转 entry

**PE32 解释器关键设计**：
- `x86emu32_state` 结构：通用寄存器、EFLAGS、段寄存器、FPU栈、512KB 栈+4MB 堆
- 内存访问：`emu->mem[addr]` 直接寻址（preferred_base=0x00400000 对应 mem 起始）
- IAT 调用：`try_iat_call` → 查找 shim 函数表 → fn32 有则直接调用，否则回退 fn64（经 ms_abi 桥接）
- `__builtin_setjmp`/`__builtin_longjmp` 用于异常处理

### Phase 2: cmd.elf

**文件**：`CODE/cmd/main.c`（~850行）

Windows cmd.exe 风格命令行，支持：
- PE/EXE 执行（通过 `reserved[4]` 获取 pe_service）
- 内置命令：dir, cls, echo, type, ver, help, exit
- PS/2 键盘输入（scan code set 1 → ASCII）
- FAT32 根目录文件列表/读取
- `C:\>` 提示符

**构建**：`build.ps1` 中添加 cmd 构建步骤，`mkfat32.ps1` 添加 `CMD ELF` 条目（8.3名 `CMD     ELF`）。

### Phase 3: Linux 兼容层

#### 3.1 park-and-resume 机制

**文件**：`CODE/UTSM/vmm/linux_resume.c/h`, `vmexit.c`, `linux_boot.c`

- Linux guest HLT 时 park（推进 RIP），不终止
- `linux_is_parked()` / `linux_resume()` 控制 guest 生命周期
- `g_guest_parked` / `g_linux_guest_active` 全局状态标志
- asm 终止路径返回到 `linux_launch()`/`linux_resume()` 保存的 RSP/返回 RIP

#### 3.2 IPC exec 协议

**文件**：`CODE/utsm-ipc/ipc_proto.h`

消息类型：`EXEC_REQUEST(0x10)` / `EXEC_STDOUT(0x11)` / `EXEC_STDERR(0x12)` / `EXEC_EXIT(0x13)` / `EXEC_READY(0x14)`

载荷结构：
```c
typedef struct {
    u32 type;
    char path[256];
    char args[512];
    u32 seq;
} ipc_exec_request;
```

#### 3.3 linux_compat 服务

**文件**：`CODE/UTSM/vmm/linux_compat.c/h`

```c
struct linux_compat_service {
    u32 magic;  // LINUX_COMPAT_MAGIC
    int (*exec)(const char *path, int argc, char **argv,
                char *stdout_buf, u32 cap, u32 *out_len, int *exit_code);
    int (*is_available)(void);
};
```

- `lxc_exec()`：构建 `ipc_exec_request` → `ipc_shm_send(EXEC_REQUEST)` → `linux_resume()` 唤醒 guest → `lxc_drain_responses()` 读取 STDOUT/STDERR/EXIT
- 通过 `reserved[5]` 暴露给 DSK/shell

#### 3.4 shell `linux` 命令

**文件**：`CODE/shell/main.c`

- `cmd_linux()`：解析参数 → 路径处理（裸命令补 `/bin/` 前缀）→ `g_lxc_svc->exec()` → 输出经 ANSI CSI 剥离后显示
- `g_lxc_svc` 从 `reserved[5]` 读取，校验 `LINUX_COMPAT_MAGIC`

#### 3.5 Linux 驱动 ioctl + exec daemon

**驱动**：`CODE/linux/patches/utsm_hcall.c`

ioctl 接口：`RECV_MSG` / `SEND_MSG` / `PARK` / `GET_READY`

**daemon**：`CODE/linux/initramfs/bin/utsm_exec_daemon.c`（静态编译）

- 接收 EXEC_REQUEST → fork+exec+pipe 捕获输出 → 发 EXEC_STDOUT/EXIT → HLT park
- 缺失时回退到 ttyS0 交互 shell

### Phase 1（重新定义）：Linux guest 真正跑起来

#### 1.1 EPT 动态页映射

**原问题**：`vmexit.c` EPT violation 只做 1:1 恒等映射 `ept_identity_map(gpa, RWX)`

**改进**：区分 RAM 区（按需分配 HPA）和 virtio-mmio 设备区（路由到设备模拟）
- RAM 区：`dma_alloc_pages` 分配 → `ept_map_page(gpa, hpa, RWX)`
- MMIO 区：解码 guest 指令 → 路由到 `virtio_mmio_handle_access`

#### 1.2 virtio-mmio 框架

**文件**：`CODE/UTSM/vmm/virtio_mmio.c/h`

GPA 布局：
```
0x10000000: virtio-blk (IRQ5)
0xF4001000: virtio-net (IRQ6)
0xF4002000: virtio-rootfs-blk (IRQ7)
```

寄存器分发：`VIRTIO_MMIO_MAGIC_VALUE` / `VERSION` / `DEVICE_ID` / `QUEUE_NUM` / `QUEUE_READY` / `QUEUE_NOTIFY` / `STATUS` / `CONFIG`

队列状态机：`avail ring` / `used ring` / `desc table` 的 guest→host 数据流

#### 1.3 virtio-blk 后端

**文件**：`CODE/UTSM/vmm/virtio_blk.c`

- `virtio_blk_backend_init()`：绑定 UTSM block 设备 0（ahci0）→ `block_get_api()` → 注册
- `blk_handle_chain()`：处理 VIRTIO_BLK_T_IN/OUT 请求 → `block_api->read/write`
- `virtio_rootfs_blk_init()`：内存后端（rootfs 镜像作为 Limine boot module）→ GPA `0xF4002000`, IRQ7

#### 1.4 vmexit dispatch 接线

- `EXIT_CPUID` → `handle_cpuid`（host passthrough + 屏蔽 VMX/hypervisor 位）
- `EXIT_IO_INSTRUCTION` → `handle_io`（COM1/PIC/PIT 模拟）
- `EXIT_INTERRUPT_WINDOW` → `maybe_inject_irq`（guest 开中断时注入 pending IRQ）
- `EXIT_VMX_PREEMPTION_TIMER` → 1ms 周期 exit → `vmexit_before_resume()`（arm timer + 串口 RX + virtio_net_poll + IRQ 注入）
- virtio IRQ 注入：QUEUE_NOTIFY 后 `vmx_guest_queue_irq(0x30+irq)` → blk→IRQ5(0x35), net→IRQ6(0x36)

#### 1.5 initramfs

**文件**：`CODE/linux/build_initrd.sh`

- busybox 1.35.0 静态下载 + 内核自带 `gen_init_cpio` 打包
- `/dev/console`、`/dev/ttyS0` 节点 + 12 个 applet symlink（含 switch_root/mkdir）
- init 脚本：daemon 缺失时回退 ttyS0 交互 shell

#### 1.6 WHPX 嵌套虚拟化问题

**问题**：WHPX 不支持嵌套 VMX，CPUID 报告 VMX 支持但 VMXON 导致 WHPX exit code 4 崩溃

**解决**：`vmx.c` 添加 `vmx_truly_available()` 检测：
1. 检查 CPUID.01H:ECX[31] hypervisor present bit
2. 检测 Hyper-V 签名 → 通过 CPUID 检查嵌套 VMX 支持
3. 检测 KVM 签名 → 允许（KVM 通常支持嵌套 VMX）
4. 未知 Hypervisor → 保守跳过

`run_qemu.bat`：WHPX 下使用 `-cpu qemu64,-vmx` 禁用 VMX CPUID 标志

### Phase 2: Linux guest Arch rootfs + pacman

#### Arch rootfs 镜像

**文件**：`CODE/linux/build_arch_rootfs.sh`

- 下载 Arch bootstrap (`.tar.zst`，121MB) → WSL 原生 ext4 解压（避免 9p 挂载性能问题）
- 裁剪：man/doc/include/locale/.a 静态库 → 597M→363M
- 编译 `utsm_exec_daemon` 静态二进制 → 植入 `/usr/local/bin/`
- 配置 pacman 国内镜像源（清华/中科大/阿里/腾讯/华为云）
- 创建 `/sbin/init`：挂载 proc/sys/dev → 启动 daemon → 交互 shell
- 静态 IP：`10.0.2.15/24` gw `10.0.2.2` DNS `10.0.2.3`（slirp 固定网段）
- `/var/lib/pacman`、`/var/cache/pacman`、`/etc/resolv.conf` 符号链接到 tmpfs（rootfs 只读）
- `SigLevel=Never` 跳过 gnupg

#### initramfs init 脚本

- 等待 `/dev/vdb` → 挂载 ext4 → `mount --move` dev/proc/sys → `exec switch_root /mnt/root /sbin/init`
- 无 rootfs 时回退到 initramfs daemon 或 debug shell

#### 内核配置

已启用：`CONFIG_VIRTIO_NET=y`、`CONFIG_VIRTIO_BLK=y`、`CONFIG_VIRTIO_MMIO=y`、`CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y`、`CONFIG_EXT4_FS=y`、`CONFIG_DEVTMPFS_MOUNT=y`

#### 构建配置调整

- `run_qemu.bat`：`-m 512M` → `-m 2G`（512MB rootfs boot module 需要足够 RAM）
- `build.ps1`：FAT32 ESP 128→768MB
- `limine.conf`：添加 `linux-rootfs.img` boot module (`linux:rootfs`)

### Phase 3: 文件传输 IPC + 网络打通

#### 3.1 文件传输 IPC

**协议**：`CODE/utsm-ipc/ipc_proto.h`

新增消息类型：`FILE_LIST_REQUEST(0x18)` / `FILE_READ_REQUEST(0x19)` / `FILE_RESPONSE(0x1A)` / `FILE_WRITE_REQUEST(0x1B)`

结构：
```c
typedef struct {
    u32 type;
    char path[256];
    u32 file_offset;
    u32 chunk_size;
} ipc_file_request;

typedef struct {
    u32 type;
    u32 total_size;
    u32 chunk_offset;
    u32 chunk_len;
    u8 data[];
} ipc_file_response;
```

**Linux 侧**：
- `utsm_hcall.c`：`POOL_WRITE`/`POOL_READ` ioctl（带边界检查）
- `utsm_exec_daemon.c`：`handle_file_list`（readdir → 文本列表）、`handle_file_read`（pread 分块）

**UTSM 侧**：
- `linux_compat.c`：`file_list`/`file_read` 服务实现（roundtrip → pool 拷出）

**shell 命令**：
- `lls [路径]`：列 Linux 目录
- `lcat <路径>`：读 Linux 文件（自动分块）
- `linux <前缀>`+Tab：补全 /usr/bin 程序名（唯一匹配补全、公共前缀扩展、多候选列表）

#### 3.2 网络打通

**virtio_net 后端**：`CODE/UTSM/vmm/virtio_net.c`

- TX：guest QUEUE_NOTIFY(1) → `net_handle_tx` → `net_gather_frame` 跳过 10 字节 `virtio_net_hdr`（支持 hdr 独立描述符和 hdr+data 合并两种布局）→ `net->tx(bound_dev)` 送 e1000 → used 回执 → IRQ6
- RX：`virtio_net_poll()` 由 preemption timer（~1kHz）周期调用 → `net_handle_rx` → `net_rx_data_region` 解析 guest buffer → `rx_poll` 填入 → hdr 清零 → used 回执 → IRQ6
- 无包时保留 buffer 不消耗（防饿死）；guest 无 buffer 时主动收包丢弃（防 e1000 RX ring 堵死）
- 继承 e1000 真实 MAC 暴露给 guest
- Feature 协商：只声明 `VIRTIO_F_VERSION_1 | MAC | STATUS`

**统计**：rx_filled / rx_drop_nobuf / rx_badbuf / tx_sent / tx_drop，每 512 事件串口打一行

### Phase 4: desktop 集成 Linux 命令为应用图标

**文件**：`CODE/tools/desktop_app.h` + `CODE/desktop/main.c`

- `app_descriptor` 增加 `linux_cmd` 字段（互斥优先级：elf_name 优先，仅设 linux_cmd 时由内置终端窗口承载）
- `MAX_APPS` 8→16、`MAX_ICONS` 8→16
- `LINUXAPP.CNF`（FAT32 根目录，8.3名 LINUXAPPCNF）：格式 `显示名|linux命令`
- `g_lxc_svc` 通过 `reserved[5]` + `LINUX_COMPAT_MAGIC` 校验
- `lnx_on_create`：bash_reset → 标题 "NAME . Linux" → `$ <cmd>` 回显 → 同步 `exec()` 阻塞 → 输出经 ANSI CSI 剥离/`^M` 忽略/`\t` 展开后一次性追加 → `[exit N]`
- `draw_icon_linux`：暗底 + 霓虹绿 `>_`

### 构建问题记录

1. **GCC 15 C23 与 Linux 6.6 不兼容**：`bool`/`false` 变成关键字 → 安装 gcc-12 + `update-alternatives`
2. **317 个 0 字节损坏 .o**：gcc-15 中断残留 → 全量清理重建
3. **C 盘满导致 WSL 崩溃**：WSL 迁移至 `D:\WSL\Ubuntu`
4. **ipc_proto.h 的 `<stdint.h>` 在 Linux 内核不可用**：条件编译 `#ifdef __KERNEL__` → `<linux/types.h>`；`#ifdef __UTSM_KERNEL__` → `<utsm/types.h>`；否则 `<stdint.h>`
5. **`my_memcpy4`/`my_memcpy_n` 未定义**：替换为 `rf_memcpy()`，修复 header 只拷 4 字节的 bug
6. **NVMe 驱动三个 bug**：Create Queue CDW10 高低半字颠倒、错误码丢失 SCT、Read opcode 误为 Flush(0x00) 应为 0x02

### WSL 构建命令（手动）

```bash
# 安装依赖
sudo apt update && sudo apt install -y build-essential bc flex bison libelf-dev libssl-dev cpio

# 下载并编译 Linux 6.6
cd ~
curl -L -o linux-6.6.tar.xz https://mirrors.tuna.tsinghua.edu.cn/kernel/v6.x/linux-6.6.tar.xz
tar xf linux-6.6.tar.xz && cd linux-6.6

# 安装 UTSM 驱动
mkdir -p drivers/utsm/include
cp /mnt/d/Code/Deshab/CODE/linux/patches/utsm_hcall.c drivers/utsm/
cp /mnt/d/Code/Deshab/CODE/linux/patches/utsm_Kconfig drivers/utsm/Kconfig
cp /mnt/d/Code/Deshab/CODE/linux/patches/utsm_Makefile drivers/utsm/Makefile
cp /mnt/d/Code/Deshab/CODE/utsm-ipc/ipc_proto.h drivers/utsm/include/
bash /mnt/d/Code/Deshab/CODE/linux/fix_driver.sh ~/linux-6.6

# 配置 + 编译
cp /mnt/d/Code/Deshab/CODE/linux/configs/utsm_x86_64_defconfig .config
make olddefconfig
make -j$(nproc) KCFLAGS='-std=gnu17' bzImage

# 拷贝产物
mkdir -p /mnt/d/Code/Deshab/SYSTEM/boot
cp arch/x86/boot/bzImage /mnt/d/Code/Deshab/SYSTEM/boot/linux-bzImage

# 构建 initramfs
cd /mnt/d/Code/Deshab/CODE/linux && ./build.sh
```

### 待端到端验证

WHPX/TCG 均不支持嵌套 VMX，需在真实硬件 / Linux KVM / 嵌套 Hyper-V VM 验证：
- `linux uname -a` — 委托执行
- `lls /usr/bin` — 列 Linux 程序
- `lcat /etc/hostname` — 读 Linux 文件
- `linux pacman -Sy` — 经 virtio_net→e1000→slirp 下载包
- `linux pac[TAB]` — Tab 补全程序名
