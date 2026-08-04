# Linux 兼容层测试矩阵

> 对应计划 Phase A：UTSM+Linux 双内核 park-and-resume 端到端验证
> 前置条件：Linux 6.6 LTS bzImage 已构建（SYSTEM/boot/linux-bzImage + linux-initrd.img）

## 状态: ⬜ PENDING

## 测试环境

- QEMU 加速: WHPX (`-accel whpx -cpu max`)，TCG 不支持 VMX
- Linux 模块: limine.conf 中 bzImage/initrd 模块已启用
- 串口: `-serial stdio`，过滤 `Linux version|utsm-linux|linux_compat|UTSM.*Linux|DSK|shell`

## 测试矩阵

| # | 测试项 | 串口/Shell 预期 | 状态 | 失败处理 | Bug# |
|---|---|---|---|---|---|
| A5.1 | UTSM VMM self-test 通过 | `[UTSM] VMM self-test PASS` | ⬜ | 检查 VMCS/EPT 初始化；BIOS 启用 VT-x | |
| A5.2 | bzImage 加载与解析 | `[UTSM] Linux bzImage loaded` | ⬜ | 检查 linux_loader_init header magic 校验 | |
| A5.3 | EPT 映射 + vmlaunch | `[UTSM] Linux guest launched` | ⬜ | 检查 EPT 表、VMCS guest 状态 | |
| A5.4 | Linux 内核启动 | `Linux version 6.6.x` 串口日志 | ⬜ | 检查 cmdline、boot_params、E820 表 | |
| A5.5 | init 脚本运行 | `[utsm-linux] init started` | ⬜ | 检查 initramfs 结构 | |
| A5.6 | exec daemon 启动 + park | `[utsm-linux] starting utsm_exec_daemon...` → `linux_compat_init() ready` | ⬜ | 检查 daemon 编译、IPC 共享内存初始化 | |
| A5.7 | DSK 正常接管 | `[DSK] boot` → `[DSK] SELFTEST PASS` | ⬜ | 验证 Linux park 不阻塞 DSK | |
| A5.8 | shell `linux`（无参）状态查询 | `Linux 兼容层已就绪 (双内核 park-and-resume)` | ⬜ | 检查 reserved[5] 传递链 | |
| A5.9 | shell `linux ls` | 输出 initramfs 根目录内容 | ⬜ | 检查 IPC exec 协议、vmresume 路径 | |
| A5.10 | shell `linux uname -a` | 输出 `Linux ... 6.6.x ...` | ⬜ | 同上 | |
| A5.11 | shell `linux cat /etc/hostname` | 输出 hostname | ⬜ | 同上 | |
| A5.12 | 错误路径：`linux nosuchprog` | 输出 exec 失败，shell 不崩溃 | ⬜ | 检查 EXEC_EXIT 消息处理 | |

## 关键约束验证

| 约束 | 验证方法 | 状态 |
|---|---|---|
| Linux guest cmdline 含 `nohlt idle=poll` | 检查 [linux_loader.c](../../CODE/UTSM/vmm/linux_loader.c) cmdline 构造 | ⬜ |
| IPC 共享内存 GPA=0x04000000, 1MB | e820 中标记 RESERVED，Linux 不分配 | ⬜ |
| Linux guest RAM 起始 0x05000000 | 检查 EPT 映射范围 | ⬜ |
| exec daemon 先发 EXEC_READY 再 HLT | daemon 源码 [utsm_exec_daemon.c](../../CODE/linux/initramfs/bin/utsm_exec_daemon.c) | ⬜ |
| UTSM hypercall ABI: RAX=magic\|op | 检查 [hypercall.c](../../CODE/UTSM/vmm/hypercall.c) 处理 | ⬜ |

## 自动化脚本

[tests/qemu/linux-compat.ps1](../../../tests/qemu/linux-compat.ps1) 动态检测 VMM 可用性，自动分流：

```powershell
.\tests\qemu\linux-compat.ps1              # QEMU WHPX：走路径 B（VMM unavailable 优雅降级）
.\tests\qemu\linux-compat.ps1 -Cpu max     # 真机/KVM nested VMX：走路径 A（完整双内核流程）
```

- **路径 A**（guest VMX 可用）：按序断言 A5.1~A5.7 启动链锚点
  （VMM init → self-test → bzImage → IPC shm → vmlaunch → `Linux version` → daemon park →
  `[LNXC] service ready` → DSK），随后经 monitor 按键执行 `dsl` / `dsl ls` / `dsl uname -a` /
  `dsl nosuchprog` 覆盖 A5.8~A5.12。
- **路径 B**（QEMU WHPX/TCG 无 nested VMX，本环境预期）：断言 `[UTSM] VMM unavailable` 后
  DSK 正常接管、dev_tests 完成、`dsl` 报告运行态不可用、Esc 到 desktop；
  A5.1~A5.12 标记 SKIP（环境限制，非产品缺陷）。

## 已知限制

- QEMU TCG 不支持 VMX，必须 WHPX 或真机
- WHPX 可能不暴露 nested VMX 给 guest（需实测）
- 若 CPU 不支持 VT-x，整个 Linux 兼容层降级为"不可用"，不影响其他系统功能

## 手动测试命令

进入 shell 后执行：

```
deshab:/ $ linux
deshab:/ $ linux ls
deshab:/ $ linux uname -a
deshab:/ $ linux cat /etc/hostname
deshab:/ $ linux nosuchprog
```

## 串口日志锚点

启动后期望按顺序出现：

```
[UTSM] VMM self-test PASS
[UTSM] Linux bzImage loaded
[UTSM] Linux guest launched
Linux version 6.6.x ...
[utsm-linux] init started
[utsm-linux] kernel: 6.6.x
[utsm-linux] starting utsm_exec_daemon...
[utsm-linux] daemon PID=...
[utsm-linux] init: daemon running, entering idle hold
[UTSM] linux_compat service ready
[DSK] boot
[DSK] SELFTEST PASS
... (进入 shell 提示符)
```
