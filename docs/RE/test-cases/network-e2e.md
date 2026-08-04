# 网络端到端测试矩阵

> 对应计划 Phase C4 网络部分：e1000 + net_stack + ping/curl/browser

## 状态: ✅ 已验证（[deshab_system_roadmap_plan.md](../../.trae/documents/deshab_system_roadmap_plan.md) Phase 1）

> 本矩阵用于回归验证，确保不退化

## 测试环境

- QEMU 网络: `-netdev user,id=net0 -device e1000,netdev=net0`
- slirp 默认: guest 10.0.2.15, 网关 10.0.2.2, DNS 10.0.2.3
- 主机 http 服务: `python -m http.server 8000`

## 测试矩阵

| # | 测试项 | 命令 | 预期 | 状态 | Bug# |
|---|---|---|---|---|---|
| N1 | e1000 驱动加载 | (自动) | `tx=ready rx=ready` | ✅ | |
| N2 | netman 注册 | (自动) | netman 打印设备枚举 | ✅ | |
| N3 | DHCP 获取 IP | (自动) | netman DHCP final rc=0 | ✅ | |
| N4 | ARP 解析网关 | (自动) | netman ARP final rc=0 | ✅ | |
| N5 | DNS 查询 | (自动) | netman DNS final rc=0 | ✅ | |
| N6 | shell ping 网关 | `ping 10.0.2.2` | 4/4 reply, RTT<10ms | ✅ | |
| N7 | shell ping 外网 | `ping 8.8.8.8` | (依赖主机联网) | 🔶 | |
| N8 | shell curl 本地 | `curl http://10.0.2.2:8000/` | HTTP 200 + 目录 HTML | ✅ | |
| N9 | shell curl 外网 | `curl http://example.com/` | HTTP 200 (依赖主机联网) | 🔶 | |
| N10 | curl.elf 工具 | `run CURL.ELF` | 独立工具执行 curl | ✅ | |
| N11 | ping.elf 工具 | `run PING.ELF` | 独立工具执行 ping | ✅ | |
| N12 | browser.elf 工具 | `run BROWSER.ELF` | URL 输入 + HTTP GET | ✅ | |

## 回归验证

每次启动 dev_mode 时，run_dev_tests 应包含：

```
[shell] dev_tests:
  ...
  ping 10.0.2.2: OK (4/4 reply, RTT=Xms)
  ...
```

若 ping 失败：

1. 检查 e1000 是否加载（`-device e1000` 参数）
2. 检查 netman 是否成功 DHCP
3. 检查 ARP 是否解析网关
4. 串口日志 grep `e1000|netman|DHCP|ARP|DNS|network`

## 性能基线

参见 [performance-baseline.md](performance-baseline.md)：

- ping RTT < 10ms
- curl 64KB 页面接收无明显停顿
- DHCP 总耗时 < 5s

## 自动化脚本

[tests/qemu/network-e2e.ps1](../../../tests/qemu/network-e2e.ps1) 分两个场景：

```powershell
.\tests\qemu\network-e2e.ps1                  # N-A + N-B 全跑
.\tests\qemu\network-e2e.ps1 -Scenarios NA    # 只跑 netman 全链路
.\tests\qemu\network-e2e.ps1 -Scenarios NB    # 只跑 curl 主机侧端到端
```

- **场景 N-A**（netman 全链路，首启调度，覆盖 N1~N5）：firstInit=`0\n0`，挂 e1000(net0) +
  virtio-net(net1) 双网卡。断言 virtio_net selftest（真实 slirp DHCP OFFER）→
  netman 双设备枚举 → DHCP DISCOVER/OFFER/REQUEST/ACK → ARP 网关解析 → DNS example.com →
  `network final rc=0x0000000000000000`，并禁止 `no OFFER / ARP timeout / DNS timeout / [PANIC]`。
- **场景 N-B**（curl.elf 主机侧端到端，覆盖 N8/N10）：dev_mode shell → 交互 `curl` →
  输入 `http://10.0.2.2:8000/`。脚本在主机 127.0.0.1:8000 起极简 HTTP 服务器
  （QemuTest.ps1 `Start-TestHttpServer`），断言主机侧收到 `GET / HTTP/1.0`
  （TCP+IP+ETH 全链路最强证据），guest 侧断言 `[curl] boot/exit` 与 shell 回收。
  HTTP 响应体渲染仅帧缓冲，标 MANUAL。
- N6/N11（ping 链路）由 [userapps-matrix.ps1](../../../tests/qemu/userapps-matrix.ps1) 交互阶段覆盖；
  N7/N9（外网）依赖主机联网，不在自动化断言范围。

## 已知限制

- HTTPS/TLS 不支持（curl 仅 http）
- TCP 仅最小 client 实现（无拥塞控制/乱序队列）
- QEMU slirp 不支持真实 Wi-Fi 模式，ath9k 驱动在 QEMU 中无设备
