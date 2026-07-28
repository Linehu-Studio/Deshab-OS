# UTSM ↔ Linux IPC 骨架 (Phase 1.3)

## 概述

Phase 1.3 实现了 UTSM 监控核与 Linux 主导核之间的双向 IPC 通信骨架，包含三个通信通道中的两个：

1. **共享内存环形缓冲区**（异步消息） — ✅ 已实现
2. **VMCALL hypercall**（同步请求/响应） — ✅ 已实现
3. **IPI 通知**（中断注入） — ⏳ Phase 1.4+

## 文件结构

### UTSM 侧（监控核）

```
CODE/UTSM/
├── include/utsm/
│   ├── ipc_shm.h         — IPC 共享内存 API 声明
│   └── hypercall.h       — VMCALL 处理器 API 声明
├── vmm/
│   ├── ipc_shm.c          — 共享内存分配、初始化、EPT 映射、ring buffer 操作
│   ├── hypercall.c        — VMCALL VM-Exit 处理（PING/HELLO/SHM_INFO/CONSOLE_WRITE）
│   ├── vmexit.c           — VM-Exit 分发器（增加 VMCALL case）
│   ├── vmexit_asm.S       — VM-Exit 汇编入口（重写：保存/恢复 guest GPR + 汇编 vmresume）
│   ├── vmm.c              — VMM 核心（g_guest_regs 全局变量 + vmm_vmexit_entry 简化）
│   └── ept.c              — EPT 页表（增加 ept_gpa_to_hpa() 翻译函数）
```

### 共享协议

```
CODE/utsm-ipc/
└── ipc_proto.h            — 双内核共享的 IPC 协议头文件
```

### Linux 侧（主导核）

```
CODE/linux/
├── configs/
│   └── utsm_x86_64_defconfig  — 增加 CONFIG_UTSM_HCALL=y
├── patches/
│   ├── utsm_hcall.c       — Linux 内核驱动：VMCALL + /dev/utsm + SHM 映射
│   ├── utsm_Kconfig       — Kconfig 配置
│   └── utsm_Makefile      — 模块 Makefile
└── build.sh               — 更新：自动安装驱动到 Linux 源码树
```

## 通信协议

### 共享内存布局（GPA 0x04000000，1MB）

```
0x04000000  utsm_ipc_shm_header (64 bytes)
            ├── magic = "UTSMHC\0\0"
            ├── version = 1
            ├── utsm_ready (UTSM 设置)
            └── linux_ready (Linux 设置)

0x04000040  utsm_ipc_ring utsm_to_linux (UTSM 写，Linux 读)
            └── 64 条消息，每条 256 字节

0x04001040  utsm_ipc_ring linux_to_utsm (Linux 写，UTSM 读)
            └── 64 条消息，每条 256 字节

0x04002040  payload pool (~1MB，用于大数据传输)
```

### VMCALL Hypercall ABI

```
Linux guest 执行 VMCALL 指令：
  RAX = UTSM_HCALL_MAGIC | op   (0x5554534D48430000 | op)
  RDI = arg0
  RSI = arg1
  RDX = arg2

UTSM 处理 VMCALL VM-Exit：
  1. 从 g_guest_regs 读取 RAX/RDI/RSI/RDX
  2. 验证 magic，提取 op
  3. 分发到对应处理器
  4. 返回值写入 g_guest_regs.rax
  5. GUEST_RIP 前进过 VMCALL 指令
  6. vmresume（GPR = 修改后的 guest 值）
```

### 已实现的 Hypercall 操作

| 操作码 | 名称 | 功能 |
|--------|------|------|
| 0x0001 | PING | 返回 "PONG" (0x504F4E47)，用于探测 UTSM |
| 0x0002 | HELLO | 握手，UTSM 回复 HELLO_ACK 消息到 ring |
| 0x0010 | SHM_INFO | 返回共享内存 GPA + size |
| 0x0020 | CAP_VALIDATE | 能力校验（stub） |
| 0x0030 | UTRW_READ | 封缄内存读（stub） |
| 0x0031 | UTRW_WRITE | 封缄内存写（stub） |
| 0x0040 | DRR_CHECKPOINT | DRR checkpoint 触发（stub） |
| 0x0050 | CONSOLE_WRITE | 将 Linux 字符串输出到 UTSM 串口 |
| 0x0051 | CONSOLE_READ | 从 UTSM 串口读取（stub） |

## 关键设计决策

### 1. VM-Exit 汇编重写

VMCS 不保存 guest GPR（RAX-R15）。原汇编只保存 host caller-saved 寄存器，无法让 C 处理器读写 guest GPR。

重写后：
- VM-Exit 时保存全部 16 个 guest GPR 到 `g_guest_regs` 结构
- 调用 C 分发器（不再从 C 调用 vmresume）
- 恢复全部 16 个 guest GPR（C 可能已修改 rax = 返回值）
- 汇编中执行 vmresume（GPR = 修改后的 guest 值）

### 2. EPT GPA→HPA 翻译

Linux guest 使用 identity-mapped 页表（GVA=GPA），hypercall 传递的指针是 GPA。UTSM 需要：
- `ept_gpa_to_hpa(gpa)` — 4 级 EPT walk，支持 4KB/2MB/1GB 页
- 翻译后加 HHDM 偏移得到 host 虚拟地址

### 3. GPA 布局修复

原 `LINUX_GUEST_INITRD_GPA = 0x04000000` 与 `UTSM_IPC_SHM_GPA = 0x04000000` 冲突。

修复后：
```
0x00400000-0x004FFFFF  固定区域 (boot_params/cmdline/GDT/PGT/stack)
0x01000000-0x02FFFFFF  Linux kernel
0x03000000-0x03FFFFFF  initrd
0x04000000-0x040FFFFF  IPC shared memory (1MB) ← e820 reserved
0x05000000-0x08FFFFFF  general RAM (64MB)
```

## 验证方法

### 1. UTSM 侧编译验证（已完成）

```powershell
cd d:\Code\Deshab\CODE\UTSM
mingw32-make
# → utsm.elf 生成，0 warnings from new code
```

### 2. Linux 内核构建（需 Linux 环境）

```bash
cd CODE/linux
./build.sh
# → SYSTEM/boot/linux-bzImage
# → SYSTEM/boot/linux-initrd.img
```

### 3. QEMU 端到端测试（需 VMX 支持）

1. 取消 `SYSTEM/limine/limine.conf` 中 Linux 模块的注释
2. 运行 `ISO/run_qemu.bat`（需 KVM 或硬件 VMX）
3. 预期串口输出：
   - `[UTSM] IPC shm init ok`
   - `[LINUX] loader init ok`
   - `[LINUX] vmlaunch`
   - Linux 启动日志
   - `[utsm] UTSM monitor detected (PONG=0x504F4E47)`
   - `[utsm] /dev/utsm registered (IPC ready)`
   - `[HCALL] HELLO from Linux guest`
4. Linux 用户态测试：
   ```bash
   echo "hello UTSM" > /dev/utsm    # → UTSM 串口显示 [LINUX] hello UTSM
   cat /dev/utsm                     # → 读取 UTSM 发来的消息
   ```

## 后续阶段

- **Phase 1.4**: virtio-console 双向通信（`/dev/hvc0` ↔ UTSM 串口）
- **Phase 1.5**: virtio-fs 文件共享（Linux 挂载 `/utsm`）
- IPI 中断注入（替代当前的轮询方式）
- UTRW/CAP/DRR hypercall 完整实现
