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

```powershell
# 运行全部自动化测试场景
.\ISO\run_tests.bat

# 输出
# ISO/logs/test-report-<timestamp>.md
```

## Bug 跟踪

所有测试发现的 bug 登记到 [../Bug记录.txt](../Bug记录.txt)，并在对应测试文档中引用 bug 编号。
