# DeshabOS安装Pacman包管理器

## 核心矛盾

pacman 安装的软件（如 `/usr/bin/firefox`）是 **Linux x86-64 动态链接 ELF**，依赖 glibc + Linux syscall ABI。Deshab 是 SAS-R0 单地址空间内核，没有 Linux syscall 层、没有动态链接器，**这些二进制在 Deshab 原生侧跑不起来**。

### pacman 依赖 vs Deshab 现状

| pacman 依赖 | Deshab 现状 |
|---|---|
| glibc + POSIX syscall | ❌ 无 POSIX 层，SAS-R0 |
| libalpm（动态链接 .so） | ❌ 无动态链接器 |
| libcurl（HTTP/HTTPS） | ❌ 网络栈未完成 |
| gpgme（签名校验） | ❌ 无加密库 |
| zlib/zstd/xz/bzip2 | ❌ 无压缩库 |
| 可写 Unix 文件层级 | ❌ FAT32 多数只读 |
| fork/exec 子进程 | ❌ shell 内建命令，无 exec/argv |

### 正确理解："WSL 互操作模式"

- Linux guest 内跑 Arch + pacman → 软件装在 Linux 的 /usr/bin
- Deshab shell/desktop 通过 IPC 委托 Linux 执行命令 → 结果显示回 Deshab
- **不是**把 Arch ELF 搬到 SYSTEM/bin

### 三种方案对比

| 方案 | 软件运行位置 | 可行性 |
|---|---|---|
| A: pacman 在 Linux guest 内自治 | Linux guest | ✅ 最简单 |
| B: 包文件物理写入 SYSTEM/bin | 想在 Deshab 原生跑 → ❌ | 不可行 |
| **C: SYSTEM/bin 放 stub，IPC 委托 Linux 执行** | Linux guest | ✅ WSL 互操作 |

---

## 实现阶段（按顺序，缺一不可）

### 第一阶段：Linux guest 真正跑起来

| 编号 | 需实现 | 现状 | 证据文件 |
|---|---|---|---|
| 1.1 | virtio-block 后端 | ❌ | `CODE/linux/configs/utsm_x86_64_defconfig:120-145` |
| 1.2 | virtio-net 后端 | ❌ | 同上 |
| 1.3 | initramfs/rootfs | ❌ 无用户态 | `CODE/UTSM/vmm/linux_loader.c:519-521` |
| 1.4 | EPT 动态页映射 | ❌ vmexit handler 只有 stub | `CODE/UTSM/vmm/vmexit.c:46-58` |
| 1.5 | 构建并测试 Linux guest | ⚠️ | `CODE/linux/README.md` |

**完成标准**: QEMU → UTSM → Linux guest shell → `uname -a`

### 第二阶段：Linux guest 内装 Arch + pacman

| 编号 | 需实现 | 说明 |
|---|---|---|
| 2.1 | Arch rootfs/bootstrap | 解压到 virtio-block 镜像 |
| 2.2 | pacman 可用 | 配置 mirrorlist |
| 2.3 | 网络可用（virtio-net） | 前提: 1.2 完成 |
| 2.4 | AUR helper（可选） | yay/paru |

**完成标准**: `pacman -S neofetch && neofetch`

### 第三阶段：UTSM↔Linux 文件传输 IPC

| 编号 | 需实现 | 现状 | 证据 |
|---|---|---|---|
| 3.1 | hypercall op: FILE_TRANSFER | ❌ 仅有9个op | `hypercall.c:202-234` |
| 3.2 | payload_pool 分块传输 | ⚠️ 1MB pool 未用 | `ipc_proto.h:30-36` |
| 3.3 | Linux 侧 utsm_hcall 扩展 | ❌ 无文件发送接口 | `utsm_hcall.c:45-84` |
| 3.4 | UTSM 侧 f32_write_root_file 接入 | ✅ 已有写入 | `dsk/main.c:150-175` |

**完成标准**: `utsm-send /usr/bin/firefox /bin/firefox` → 写入 Deshab FAT32

### 第四阶段：Deshab shell 命令委托执行

| 编号 | 需实现 | 现状 | 证据 |
|---|---|---|---|
| 4.1 | hypercall op: EXEC_FORWARD | ❌ | `hypercall.c` |
| 4.2 | Linux 进程执行 + stdout/stderr 回传 | ❌ | 需新增 |
| 4.3 | shell path_search_and_run 扩展 | ⚠️ 只能加载原生 PIE | `shell/main.c:1416-1460` |
| 4.4 | shell IPC 调用层 | ❌ | 需新增 |

**完成标准**: shell 输入 `firefox` → Linux 执行 → 输出回显

### 第五阶段：desktop 集成（可选）

- 5.1: desktop 动态注册"Linux 命令"为应用图标（扩展 app_descriptor.linux_cmd）
- 5.2: 点击图标 → EXEC_FORWARD hypercall

---

## 最小可用路径

1. 第一阶段全部完成（Linux guest 启动到 shell）
2. 第二阶段 2.1-2.3 完成（Arch + pacman + 网络）
3. 第三阶段 3.1-3.4 完成（文件从 Linux 写入 Deshab FAT32）
4. 第四阶段 4.1-4.4 完成（shell 命令委托执行）

**当前最大瓶颈**: 第一阶段的 virtio 后端 + initramfs

---

## 系统现状检查

### Linux guest 方向

| 检查项 | 状态 |
|---|---|
| Linux 内核源码 | ❌ 未克隆 |
| linux-bzImage | ❌ 未构建 |
| initramfs/rootfs | ❌ 不存在 |
| limine.conf 配置 | ⚠️ 已注释（正确） |

### 已知 Bug

| 编号 | 严重度 | 问题 |
|---|---|---|
| BUG-001 | P2 | Files 窗口关闭按钮无法关闭 |
| BUG-002 | P2 | 开发者模式 shell 命令回车无反应 |
| BUG-003 | P2 | 桌面 shell 命令文字跑到下一行 |
| BUG-004 | P2 | Files 显示的不是 SYSTEM/user 目录 |
| BUG-005 | P2 | Files 没有文件夹图标 |
| BUG-006 | P2 | Files 缺少 ".." 返回上级 |
| BUG-007 | P3 | 桌面不需要重复显示终端和 CMD 图标 |
| BUG-008 | P3 | 打字机效果需要再快3倍 |
