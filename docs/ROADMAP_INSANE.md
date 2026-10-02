# Deshab-OS「疯起来」填补计划 v1.0

> 总原则：先让"疯"有可演示的实体，再让疯自洽不翻车。
> 状态规范：✅ 已实现（附 QEMU 证据） · 🔶 演示级（注明边界） · ⬜ 仅设计（不承诺）
>
> **编号体系（唯一任务编号）**：F=骨架 / U=仪式 / D=自由 / R=落款 / E=品牌。
> Phase 0–9 仅作历史存档（见 [ROADMAP.md](./ROADMAP.md)）；M 编号仅指 CLAUDE.md 里程碑记录。

---

## 分期总览

```text
一期 骨架 → 让"疯"站得住（基础设施）
二期 仪式 → 让 UTSM/UTRW 成型
三期 自由 → 让 DKM/用户态有入口
四期 落款 → 让 DRR/PCQ 可演示
五期 品牌 → 让疯变成说明书
```

---

## 🔩 一期：让「疯」站得住

没有这层，"无限自由"只是内核随便崩。

| 编号 | 功能 | 疯的意义 | 状态 | 验收标准 |
|------|------|---------|------|---------|
| F1 | 内核 panic 统一兜底 | 崩了也得崩出莲花 | ✅ | panic_symbol + 莲花崩溃屏 + 寄存器简表 + DRR 归档后 halt |
| F2 | UEFI 帧缓冲控制台 | 疯要有画面 | 🔶 | fbcon.h 抽象已落（panic 路径使用）；统一控制台后端 ⬜ |
| F3 | 构建跨平台（Make/CMake） | 疯不应绑 Windows | 🔶 | 根 Makefile：Linux `make dev/release/realtest` 通过（编译+注入+pack）；macOS 打包需 mkfs.ext4 替代 |
| F4 | 状态机自检 boot_ok | 知道自己是几成疯 | ✅ | 启动分级自检（IDT/UTSM/DKM/调度器/DRR），输出 ✅/🔶 明细 |
| F5 | 可跳过 FirstInit | 自由的第一步 | ✅ | FUCK `[dsk] skip_firstinit=1` 直达 desktop；build/configs 模板注入生效 |

一期不做任何"安全/加密"，只让系统稳定地发疯。

---

## 🗝️ 二期：让「仪式」成型（UTSM / UTRW 落地）

Deshab 最标志性的疯点——给空气上锁。

| 编号 | 功能 | 疯的意义 | 状态 | 实现要点 / 验收标准 |
|------|------|---------|------|-------------------|
| U1 | UTSM 元数据区（UUID 表） | 每页都有"身份" | ✅ | 封缄段表 + UUID 调谐已就位（`CODE/UTSM/core/segment.c`），可查询页归属 |
| U2 | UTRW Fast/Slow Path 真分支 | 快慢路不是画出来的 | ✅ | Fast path ✅；Slow path ✅（EPOCH/KEY_MISS 恢复，MAC_FAILED/POISONED 交 DRR，selftest PASS） |
| U3 | `@sealed` 段可选标记 | 用户能选锁不锁 | ✅ | `UTSM_SEG_F_UNSEALED` 明文直存（0 加解密开销）+ linker.ld `.sealed` section（`__sealed_start/end`）；selftest PASS |
| U4 | 密文页 dump 工具 | 发疯也要可调试 | ✅ | `utrw_debug_dump_page`：串口 64B 密文/明文 hex 对照，selftest 内置演示（QEMU PASS） |
| U5 | UTSM selftest 全覆盖 | 把🔶刷成✅ | ✅ | chacha20 KAT = RFC 8439 §2.4.2 官方向量（三方对拍）；"间歇 FAIL" 确认为幽灵缺陷（从未合并进树）；KAT+MAC 确定性+段表+越界+调度+DRR 全量稳定 PASS |

**二期完成：UTSM 从"设计"变成"可开关的特性"，疯得有旋钮。**

---

## 🔌 三期：让「自由」有入口（DKM / 用户态雏形）

无限自由的兑现层。

| 编号 | 功能 | 疯的意义 | 状态 | 实现要点 / 验收标准 |
|------|------|---------|------|-------------------|
| D1 | DKM 热卸载（基础） | 从"焊死"到"可拔" | ⬜ | 仅有 `DKM_F_NO_UNLOAD` 标志与状态机文档；需引用计数+资源回收，load/unload 循环 100 次不崩 |
| D2 | 裸机器码加载器 | 真·无限自由 | ⬜ | 任意 elf/裸码塞地址即执行；能打印、能崩内核 |
| D3 | 内核 API 导出表 | 自由也要有地图 | ⬜ | 当前为固定偏移 struct ABI（`CODE/sdk/include/deshab/kernel_api.h`）；需按名导出表 kapi() |
| D4 | DKM 彩色日志前缀 | 每个驱动有自己的脾气 | ✅ | 驱动名 FNV hash → 8 色 ANSI（确定性可复现）；init 期着色，仅串口；FUCK `color_log` 开关 |
| D5 | 最小 REPL/shell | 自由的交互面 | ✅ | shell.elf：ls/cat/cp/mv/rm/run/probe/test/pci 等 25+ 命令 |

三期做完：用户能真在系统里"为所欲为"，疯得可上手。

---

## ⏪ 四期：让「疯但有落款」落地（DRR / PCQ）

把"未实现"的⬜变成"能演示"。

| 编号 | 功能 | 疯的意义 | 状态 | 实现要点 / 验收标准 |
|------|------|---------|------|-------------------|
| R1 | checkpoint 快照 | 时光键雏形 | ✅ | dirty shard 位图收集 → dma 页快照 + A/B 双槽 + CRC64 + 原子切换（`CODE/UTSM/drr/drr_core.c`） |
| R2 | 看门狗计时器 | 卡死也能体面 | ✅ | register/kick/timeout → fault 路径 → 回滚/复位；负向测试 PASS（demo_fault=1） |
| R3 | Emergency Pool | 崩溃后有地方喘气 | ✅ | 编译期 64KB 独立静态池（独立于堆）+ bump 分配器；`drr_emergency_pool_kb`/`drr_recovery_log_size` FUCK 接线 ✅（运行期钳位） |
| R4 | A/B 镜像回滚 | 时光机第 1 台 | 🔶 | 页级回滚 + CRC verify + BLAKE2b keyed 页 MAC ✅；segment/system 级回滚待真实加密验证 |
| R5 | PCQ 位图调度器（单核→多核） | 让"凭感觉"变 O(1) | 🔶 | O(1) 位图 + LAPIC tick + xv6 式切换 ✅；BSP-only，per-cpu 多核负载均衡 ⬜ |

四期做完：DRR/PCQ 从⬜变🔶，疯得有章法。

---

## 🪷 五期：让「疯」变成品牌（体验/文档/仪式）

不是功能，是"Deshab 味"。

| 编号 | 功能 | 疯的意义 | 状态 | 验收标准 |
|------|------|---------|------|---------|
| E1 | 启动莲花动画 | 每次开机都有仪式 | ⬜ | Limine→UTSM 间莲花淡入（当前为 DSK 静态 Logo） |
| E2 | "假想时光键"彩蛋 | 未实现的浪漫 | ⬜ | 按键弹出"假设已回滚至假设态" |
| E3 | 提交信息规范（疯洁版） | 把脏历史变规矩 | ⬜ | 禁非描述性提交；保留疯味措辞 |
| E4 | deshab-fuckyou-edition 风味 ISO | 官方下场整活 | ⬜ | 独立分支，README 带莲花 slogan |
| E5 | 疯圣典进 docs/ | 疯得有文档 | ✅ | [PHILOSOPHY.md](./PHILOSOPHY.md) 已合并 |

---

## ⚠ 保命守则

1. UTSM 二期必须做成可关闭，否则三期 DKM 乱摸内存会成调试地狱。
2. DRR 先"单核+内存快照"，QEMU 里验证，别一上真机。
3. PCQ 别和 UTRW 缓存同时改，一次只动一个子系统。
4. 每个里程碑必须有 QEMU 证据（串口日志/截图），疯要有画面证据。

> 一句话：先让 Deshab 稳稳地疯，再让它疯得能演示，最后让它疯得有说明书。🪷
