# Deshab Linux/EXE 兼容层开发

## 架构总览

```
桌面命令行1: shell.elf (原生系统命令行 + linux 命令委托执行)
桌面命令行2: cmd.elf  (Windows 风格命令行 + PE/EXE 兼容)
```

三个垂直层次：
1. **虚拟化底座**：UTSM 作为 Type-1 hypervisor，VMX/EPT 完整虚拟化，park-and-resume 机制让 Linux guest 与 DSK 时分复用 CPU
2. **双兼容引擎**：Linux 兼容路径（IPC 委托 Linux guest 执行）+ PE/EXE 兼容路径（UTSM 内置 PE loader + x86emu32 解释器）
3. **用户界面与文件系统**：shell.elf / cmd.elf / desktop.elf / FAT32 / initramfs+virtio-blk

## 一、PE/EXE 兼容层（UTSM 内置，`CODE/UTSM/pe/`）

### 1. pe_service.c / pe.h — 统一服务入口
- 全局 service table（函数指针表）：`pe_load()` / `pe_run()` / `pe_unload()`
- PE32+ → preferred_base 加载，Ring0 原生执行（MS x64 ABI）
- PE32 → 专用大缓冲区，x86emu32 解释执行
- 通过 `dsk_boot_context.reserved[4]` 暴露给 DSK

### 2. pe_loader.c — PE 解析/映射/重定位
- DOS Header → PE Header → Section Table 解析
- PE32+：`__attribute__((ms_abi))` shim，原生 Ring0 执行
- PE32：无需重定位，直接 preferred_base 加载
- `apply_relocations(image_size)` — base relocation 处理
- `pe_get_iat_entries()` — 暴露 IAT 给 shim 注册
- import 解析 → 查 shim 表 → 填充 IAT

### 3. pe_shim.c / pe_shim.h — Windows API 拦截
- IAT 拦截：按 DLL!Function 名查 shim 表填充函数指针
- shim 函数签名用 `__attribute__((ms_abi))`
- `__builtin_setjmp`/`__builtin_longjmp`（`void **`）用于 PE 入口异常恢复
- 每条 IAT 条目可注册 `fn32`（PE32）和 `fn64`（PE32+），`try_iat_call` 在 fn32=NULL 时回退到 fn64

### 4. x86emu32.c / x86emu32.h — x86-32 指令解释器
- 支持指令：MOV/ADD/SUB/AND/OR/XOR/CMP/TEST, SHL/SHR/SAR（含 CL 变长）, PUSH/POP, JMP/Jcc/CALL/RET, MOVSX/MOVZX, rep_prefix, ModR/M+SIB 解码
- `emu->mem` 指向大缓冲区起始（虚拟地址 0），`mem[preferred_base + rva]` 正确寻址
- `try_iat_call` — 调用 IAT 中的 shim 函数

### cmd.elf（`CODE/cmd/main.c`）
- Windows cmd.exe 风格命令行，从 `reserved[4]` 读取 `pe_svc`
- 内置命令：dir, cd, type, copy, del, exit 等
- FAT32 I/O 通过 `f32_list_root()` 回调 API
- Esc 键返回 DSK，支持从 FAT32 root 加载 PE 文件执行

## 二、Linux 兼容层（双内核 park-and-resume）

### GPA 布局

| GPA 区间 | 用途 |
|---------|------|
| `0x00400000 - 0x02FFFFFF` | initrd / boot_params |
| `0x03000000 - 0x03FFFFFF` | Linux guest initrd |
| `0x04000000` | IPC 共享内存（1MB） |
| `0x05000000+` | Linux guest RAM（64MB） |
| `0x01000000` | Kernel 映射 |
| `0x00600000` | GDT |

### VMCS / VMEXIT 处理
- **vmexit_asm.S**：保存 16 个 guest GPR 到 `g_guest_regs`，调用 C `vmexit_dispatch()`
- dispatch 返回 0 → `vmresume`；返回非 0 → 恢复 host RSP/返回 RIP
- `g_saved_host_rsp` / `g_saved_return_rip` 全局变量（vmm.h 声明，vmm.c 定义）

**已实现 exit reason**：

| Exit Reason | 处理 |
|-------------|------|
| EXIT_HLT | park-and-resume：self-test 终止，Linux guest park |
| EXIT_EPT_VIOLATION | 按需分配 HPA，区分 RAM/MMIO |
| EXIT_CPUID | host passthrough + 屏蔽 VMX 位 |
| EXIT_IO_INSTRUCTION | COM1/PIC/PIT 模拟 |
| EXIT_RDMSR/WRMSR | MSR 读写处理 |
| EXIT_VMCALL | 9+ hypercall 分发 |
| EXIT_INTERRUPT_WINDOW | guest 开中断时注入 pending IRQ |
| EXIT_VMX_PREEMPTION_TIMER | 1ms 周期 exit |

### park-and-resume 机制
- `handle_hlt` 区分 self-test（终止）和 Linux guest（park）
- Linux guest HLT → 推进 guest RIP → `g_guest_parked = 1` → 返回非 0 → asm 恢复 host 上下文
- `linux_is_parked()` 查询状态，`linux_compat_exec()` 调用 `linux_resume()` 唤醒
- `linux_launch()` 成功后设 `g_guest_parked = 1`

### IPC 协议

**Hypercall ABI**：guest RAX = 操作码，RDI/RSI/RDX = 参数，返回值写入 guest RAX
- VMCALL 魔数 `0x5554534D48430000`（"UTSMHC\0\0"）
- 9 种基础 op：PING/HELLO/SHM_INFO/CAP_VALIDATE/UTRW_READ/UTRW_WRITE/DRR_CHECKPOINT/CONSOLE_WRITE/CONSOLE_READ

**IPC EXEC 协议**（消息号 16-20）：

| 消息号 | 常量 | 用途 |
|-------|------|------|
| 16 | UTSM_MSG_EXEC_REQUEST | shell 请求 Linux 执行命令 |
| 17 | UTSM_MSG_EXEC_STDOUT | daemon 回传 stdout |
| 18 | UTSM_MSG_EXEC_STDERR | daemon 回传 stderr |
| 19 | UTSM_MSG_EXEC_EXIT | daemon 回传退出码 |
| 20 | UTSM_MSG_EXEC_READY | daemon 就绪通知 |

`ipc_exec_request` 结构体约束 ≤240 字节（适配 IPC 单消息槽）。

### linux_compat 服务（`CODE/UTSM/vmm/linux_compat.c`）
- `exec()` 实现：构建 `ipc_exec_request` → `ipc_shm_send()` → `linux_resume()` → 读取响应
- 通过 `dsk_boot_context.reserved[5]` 暴露

### shell `linux` 命令（`CODE/shell/main.c`）
- `cmd_linux` 函数：`linux ls -la` → `g_lxc_svc->exec("/bin/ls -la")`
- 支持绝对路径，Linux 未构建时显示"兼容层不可用"

### virtio 模拟
- 模型：virtio-mmio（非 PCI）
- **IRQ 路由**：virtio-blk → IRQ5(0x35)，virtio-net → IRQ6(0x36)，公式 `IRQ_vector = 0x30 + irq`
- MMIO 处理：EPT misconfig exit → 解码 guest 指令访问大小/数据 → 寄存器分发
- `QUEUE_NOTIFY` 后调用 `vmx_guest_queue_irq(0x30+irq)` 注入中断
- `maybe_inject_irq` 按 guest IMR/IF 决定投递时机

**virtio-blk**：桥接到 UTSM block API（ahci0 SATA 盘），8GB 容量
**virtio-net**：桥接到 UTSM net API，TX/RX ring 实现占位
**virtio-rootfs-blk**：memory-backed 后端，从 Limine boot module 加载 Arch rootfs 镜像

### EPT 动态页映射
- 4 级 EPT 页表 walk，支持 4KB/2MB/1GB
- `ept_identity_map(gpa, RWX)` — 1:1 恒等映射
- `ept_gpa_to_hpa(gpa)` — hypercall 访问 guest 内存
- EPT violation handler：RAM 区按需分配 HPA，MMIO 区路由到设备模拟

### Linux guest 启动流程（`kernel/main.c`）
1. `vmm_init()` — 启用 VMX、初始化 EPT、分配 VMCS
2. `vmm_self_test()` — 自测 guest vmlaunch
3. `ipc_shm_init()` — 分配 1MB 共享内存、EPT 映射到 GPA 0x04000000
4. `linux_loader_init()` — 加载 bzImage、setup boot_params、e820 映射
5. `linux_launch()` — 配置 VMCS、`virtio_mmio_init()`、arm preemption timer、`vmlaunch`
6. `linux_compat_init()` — 初始化 Linux 兼容服务
7. `dsk_load_and_jump()` — 加载 DSK 主内核

**cmdline 关键参数**：`nohlt` + `idle=poll`（禁止 guest HLT idle），仅 daemon 显式 park 时触发 UTSM resume

### Linux exec daemon（`CODE/linux/initramfs/bin/utsm_exec_daemon.c`）
- 静态编译，打包进 initramfs
- 流程：fork → exec 命令 → pipe 捕获 stdout/stderr → IPC 发送 EXEC_STDOUT/STDERR/EXIT → HLT park
- 缺失时 init 回退到 `setsid cttyhack sh`（ttyS0 交互 shell）

### UTSM driver（`CODE/linux/patches/utsm_hcall.c`）
- `/dev/utsm` 字符设备，VMCALL hypercall 封装
- SHM 映射（mmap 共享内存 GPA 0x04000000）
- ioctl 接口：RECV_MSG / SEND_MSG / PARK / GET_READY

## 三、WSL 构建 Linux 内核

### 构建步骤
```bash
# WSL 内执行
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
make -j$(nproc) KCFLAGS='-std=gnu17' bzImage   # KCFLAGS 修复 gcc 15 C23 兼容

# 拷贝产物
mkdir -p /mnt/d/Code/Deshab/SYSTEM/boot
cp arch/x86/boot/bzImage /mnt/d/Code/Deshab/SYSTEM/boot/linux-bzImage
```

### 关键修复
1. **ipc_proto.h 条件编译**：`<stdint.h>` 在 Linux 内核不可用，需 `#ifdef __UTSM_KERNEL__` / `#include <linux/types.h>` 分支
2. **gcc 15 C23 兼容**：`bool`/`false` 变关键字，需 `KCFLAGS='-std=gnu17'`；顶层 Makefile `-std=gnu11` 改为 `-std=gnu17`；`arch/x86/boot/compressed/` 也需同步修改
3. **WSL ext4 I/O 错误**：WSL 虚拟磁盘可能损坏，需 `wsl --shutdown` + `wsl --unregister Ubuntu` + 重装
4. **WSL 网络问题**：GitHub 访问被墙，需用清华镜像下载内核 tarball

### initramfs 构建（`build.sh` / `build_initrd.sh`）
- busybox 1.35.0 静态二进制下载（缓存 `.cache/`）
- 内核自带 `gen_init_cpio` 打包（无 root、无 cpio 依赖）
- spec 声明 `/dev/console`、`/dev/ttyS0` 节点 + 14 个 applet symlink（含 switch_root、mkdir）
- daemon 静态编译
- init 脚本：daemon 缺失时回退 `setsid cttyhack sh`（ttyS0 交互 shell，bring-up 用）

## 四、WHPX 嵌套虚拟化问题

**问题**：WHPX 下 CPUID 报告 VMX 支持但 VMXON 不可用，导致 QEMU exit code 4 不可恢复崩溃。

**解决方案**（`vmx.c` `vmx_truly_available()`）：
1. 检查 CPUID.01H:ECX[31] hypervisor present bit
2. 检测 Hyper-V 签名 → 通过 CPUID 检查嵌套 VMX 支持 → 无则跳过
3. 检测 KVM 签名 → 允许（KVM 通常支持嵌套 VMX）
4. 未知 Hypervisor → 保守跳过
5. `run_qemu.bat`：WHPX 下使用 `-cpu qemu64,-vmx` 禁用 VMX CPUID 标志

**QEMU 验证结果**：UTSM → VMM init → 检测 Hyper-V 无嵌套 VMX → 安全跳过 → 加载 DSK → shell.elf 正常运行

## 五、Arch rootfs 构建（第二阶段）

### build_arch_rootfs.sh 关键步骤
1. 下载 Arch bootstrap tarball（`.tar.zst` 格式，需 `zstd` 包）
2. 解压到 WSL 原生 ext4（`/tmp`，避免 9P 慢速 I/O）
3. 编译 utsm_exec_daemon 静态二进制并安装到 rootfs
4. stripping：删除 locale(109M)/man(37M)/include(39M)/doc(13M)/info(7.3M)/.a libs → 597M→363M
5. 创建 512MB ext4 镜像，rsync rootfs 内容
6. 拷贝最终 `rootfs.img` 到 `SYSTEM/boot/`

### 内核配置确认
- `CONFIG_EXT4_FS=y`, `CONFIG_VIRTIO_BLK=y`, `CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y`, `CONFIG_DEVTMPFS_MOUNT=y`, `CONFIG_VIRTIO_NET=y`

### init 脚本修改
- 挂载 `/dev/vdb`（Arch rootfs）→ `switch_root` 到 Arch
- 保留 phase-1 回退：无 rootfs 时 ttyS0 交互 shell

### limine.conf 修改
- 添加 `linux-rootfs.img` boot module
- cmdline 添加第三个 `virtio_mmio.device` 条目

## 六、Pacman 包管理方案（WSL 互操作模式）

### 核心矛盾
- pacman 依赖 glibc + POSIX syscall + libalpm + libcurl + gpgme + zlib/zstd/xz + 可写 Unix 文件层级 + fork/exec
- Deshab 是 SAS-R0 单地址空间 Ring0，无 POSIX 层、无动态链接器、无 syscall 入口

### 方案 C（推荐）：IPC 委托执行
- `SYSTEM/bin/` 放 stub，PATH 命中后通过 IPC 委托 Linux guest 执行 `/usr/bin/xxx`
- Linux 程序在 Linux guest 内原生执行，依赖 glibc + Linux syscall ABI

### 五阶段实现

| 阶段 | 核心任务 | 状态 |
|------|---------|------|
| 第一阶段 | Linux guest 跑到 shell | 代码就绪，需真机 VT-x 验证 |
| 第二阶段 | Arch rootfs + pacman + 网络 | rootfs 已构建，需 VMX 环境验证 |
| 第三阶段 | FILE_TRANSFER hypercall + payload_pool 分块传输 | 未实现 |
| 第四阶段 | EXEC_FORWARD hypercall + shell 委托执行 | 未实现 |
| 第五阶段 | desktop 动态注册 Linux 命令图标 | 未实现 |

## 七、已实现 vs 待实现

### 已实现
- PE 兼容层完整（pe_service/loader/shim/x86emu32），零警告
- cmd.elf + shell.elf `linux` 命令 + desktop 双命令行
- park-and-resume + IPC exec 协议（消息 16-20）+ linux_compat 服务
- Linux driver ioctl（RECV_MSG/SEND_MSG/PARK/GET_READY）+ exec daemon
- virtio-mmio 框架 + virtio-blk/net/rootfs-blk 后端 + EPT 动态页映射
- vmexit dispatch 接线（CPUID/IO/INT_WINDOW/PREEMPTION_TIMER）+ virtio IRQ 注入
- WHPX 安全 VMX 检测 + 优雅回退
- build.sh busybox initramfs 打包 + Arch rootfs 构建脚本
- Linux bzImage(5.7MB) + initrd(1MB) + rootfs.img(512MB stripped) 已构建
- FAT32 镜像含 CMD ELF

### 待实现
- 真机 VT-x / KVM 环境验证 Linux guest 启动
- FILE_TRANSFER / EXEC_FORWARD hypercall
- 完整 Win32 API + .NET API 覆盖
- virtio-net 完整 TX/RX ring
- Arch rootfs 在 VMX 环境中实际运行验证
- pacman 镜像源配置 + AUR helper
- desktop 动态注册 Linux 命令图标
- FAT32 ESP 容量扩展（当前 128MB 不够容纳 rootfs.img）
