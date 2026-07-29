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

## 已知限制

- HTTPS/TLS 不支持（curl 仅 http）
- TCP 仅最小 client 实现（无拥塞控制/乱序队列）
- QEMU slirp 不支持真实 Wi-Fi 模式，ath9k 驱动在 QEMU 中无设备
