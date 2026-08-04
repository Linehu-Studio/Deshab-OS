# Deshab 测试用例矩阵

本目录组织 Deshab-OS 的系统化测试用例文档。

## 测试矩阵总览

| 文档 | 范围 | 优先级 |
|---|---|---|
| [boot-regression.md](boot-regression.md) | 启动链路回归（首启/正常/dev_mode/无盘/无NIC） | P0 |
| [linux-compat.md](linux-compat.md) | UTSM+Linux 双内核兼容层端到端 | P0 |
| [pe-compat.md](pe-compat.md) | PE/EXE 兼容层（PE32+ native + PE32 x86emu32） | P1 |
| [driver-matrix.md](driver-matrix.md) | 14 DKM 驱动功能矩阵 | P1 |
| [userapps-matrix.md](userapps-matrix.md) | 用户态程序矩阵（shell/cmd/desktop/tools） | P1 |
| [network-e2e.md](network-e2e.md) | 网络端到端（DHCP/ARP/DNS/ping/curl） | P1 |
| [performance-baseline.md](performance-baseline.md) | 性能基线（TSC 测量） | P2 |
| [x86emu32-coverage.md](x86emu32-coverage.md) | x86emu32 指令覆盖率 | P2 |

## 测试场景状态标记

- ✅ PASS: 全部测试项通过
- 🔶 PARTIAL: 部分测试项通过，存在已知限制
- ❌ FAIL: 关键测试项失败，需修复
- ⬜ PENDING: 未执行
- ⏭ SKIP: 因环境/依赖不可用而跳过

## 自动化执行

PowerShell 5.1 测试套件位于 [tests/qemu/](../../../tests/qemu/)，每份矩阵文档的"自动化脚本"小节
列出对应用例脚本与参数。总运行器顺序执行全部 9 个用例并汇总：

```powershell
# 全量（先构建镜像再跑 9 用例）
.\tests\qemu\Run-All.ps1 -Build

# 复用现有镜像全量跑
.\tests\qemu\Run-All.ps1

# 只跑指定用例（逗号分隔，大小写不敏感）
.\tests\qemu\Run-All.ps1 -Only network-e2e,x86emu32-coverage
```

| 用例脚本 | 对应文档 | 说明 |
|---|---|---|
| boot-regression.ps1 | [boot-regression.md](boot-regression.md) | B1~B5 启动场景（-Scenarios 过滤） |
| driver-matrix.ps1 | [driver-matrix.md](driver-matrix.md) | 16 清单驱动加载矩阵 + NVMe 写路径 |
| linux-compat.ps1 | [linux-compat.md](linux-compat.md) | VMM 分流（-Cpu max 走路径 A） |
| pe-compat.ps1 | [pe-compat.md](pe-compat.md) | PE32+ 原生 + PE32 x86emu32 |
| deaicup-e2e.ps1 | [pe-compat.md](pe-compat.md) | Deaicup GUI PE 端到端（首帧像素/键鼠消息/Esc 退出链） |
| userapps-matrix.ps1 | [userapps-matrix.md](userapps-matrix.md) | shell dev_tests + id/ping/editor/fileman/browser 全交互（sendkey 驱动 + Esc 退出链） |
| network-e2e.ps1 | [network-e2e.md](network-e2e.md) | N-A netman / N-B curl（-Scenarios NA,NB） |
| performance-baseline.ps1 | [performance-baseline.md](performance-baseline.md) | 启动阶段墙上计时（-Runs N） |
| x86emu32-coverage.ps1 | [x86emu32-coverage.md](x86emu32-coverage.md) | 静态枚举 + hello32 动态执行 + shim/返回链硬断言（末锚点被 cmd AUTOEXEC 缓冲别名系统 bug 阻塞，见文档） |

公共库 [tests/qemu/lib/QemuTest.ps1](../../../tests/qemu/lib/QemuTest.ps1) 提供 QEMU 会话管理、
串口日志断言（必须/禁止/顺序/计数）、monitor sendkey/screendump、firstInit/AUTOEXEC 场景
备份-替换-恢复、极简 HTTP 测试服务器。所有 QEMU 启动带 `-snapshot`（镜像只读，避免写锁与并发冲突；
可用 `-NoSnapshot` 关闭），monitor 端口按用例固定分配（boot-regression=45501、
driver-matrix=45502、linux-compat=45503、pe-compat=45504、deaicup-e2e=45454、
userapps-matrix=45505、network-e2e=45506、performance-baseline=45507、x86emu32-coverage=45508；
被占用时段内自动退让，兜底 45509-45599），避开人工调试 4444 与其他套件 456xx/457xx。清理策略只杀本套件 `-name deshabtest-` 标记的会话，
绝不全局杀 QEMU（并发代理/人工调试不受影响）。

并发隔离（套件可与其他代理/人工的 QEMU+build 并发运行）：

- QEMU 只挂私有镜像副本：deshab/nvme/usb 镜像在 `Initialize-QemuTest` 时复制到
  `.build_tmp\tests\img\`（share-tolerant 读取 + 60s 重试；mtime+size 未变则跳过）；
- 场景翻转（firstInit.txt / AUTOEXEC.BAT）只写 `.build_tmp\tests\SYSTEM\` 镜像树
  （robocopy /MIR 自共享 SYSTEM 同步），SATA 镜像由 `.build_tmp\tests\fat32gen\` 下的
  mkfat32.ps1 副本生成到私有路径——共享 SYSTEM 树与 `.build_tmp\sata_fat32_dsk.img`
  全程不被套件读写；
- 会话早夭自愈：WHPX 分区强杀后偶发重建异常（guest 启动即三重故障，日志仅一行
  `Ignoring request for interrupt vector 0`），启动 3s 内死亡且日志 <4KB 时自动重开一次。

临时文件与日志输出到 `.build_tmp\tests\`（Run-All 汇总日志在 `.build_tmp\tests\runall\`，
性能基线 JSON 在 `.build_tmp\perf_baseline.json`）。每用例独立子进程 + 场景恢复，幂等可重复执行。

退出码约定：用例全部 PASS → 0；任一 FAIL → 1。MANUAL/SKIP 项不影响退出码。

## Bug 跟踪

所有测试发现的 bug 登记到 [../Bug记录.txt](../Bug记录.txt)，并在对应测试文档中引用 bug 编号。
