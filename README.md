# Deshab-OS

> 实验内核 · QEMU 可启动演示 · 非生产系统

Deshab-OS 是一个从零编写的独立 64 位操作系统，基于 x86_64 long mode，采用单地址空间 Ring0（SAS-R0）内核架构，配套封缄内存（UTSM）、专用恢复根（DRR）与可加载驱动模块系统（DKM）。

当前已完成可启动的实验内核闭环：Limine 引导 → UTSM 首阶段内核 → 14 个 DKM 驱动加载 → DSK 主内核 → 桌面/登录/向导。

## 核心架构

```text
SAS-R0-PCQ  单地址空间 Ring0 + Per-CPU O(1) 位图调度器（当前 BSP-only）
UTSM        UUID 调谐封缄内存（明文须经 UTRW 读写器）
UTRW        带标记读写器（Fast Path ✅ / Slow Path ⬜）
DRR         专用恢复根（看门狗 / checkpoint / A-B 回滚 / 独立 Emergency Pool）
DKM         驱动模块系统（ELF64 .drv ABI，4 stage 分阶段加载）
```

## 项目状态

**唯一状态源见 [docs/STATUS.md](docs/STATUS.md)**，本表只是引用摘要。

| 子系统 | 状态 |
|--------|------|
| 早期启动 / IRQ / DMA / DKM 14 驱动 | ✅ |
| UTSM 封缄（Fast path + selftest） | 🔶 |
| AHCI / e1000 / FAT32 写盘 | ✅ |
| NVMe / virtio-net / FAT32 完整路径 | 🔶 |
| PCQ 调度器 / DRR 恢复根 | ✅（BSP-only / 演示级） |
| Five-Phase 填补计划 | 见 [docs/ROADMAP_INSANE.md](docs/ROADMAP_INSANE.md) |

### 已知边界（Non-Goals）

1. **无进程隔离**：SAS-R0 单地址空间 Ring0，驱动 bug = 内核全盘崩。这不是安全 OS，是敞开工坊。
2. **UTSM 仅 selftest 覆盖**：封缄是"设计意图 + 演示级实现"，不是机密计算。
3. **驱动为演示级**：QEMU 能演示 ≠ 真机可依赖；NVMe/virtio-net 尚未走完数据路径。

## 文档

| 文档 | 内容 |
|------|------|
| [STATUS.md](docs/STATUS.md) | **单一状态源**（全子系统三色表 + 证据） |
| [ROADMAP_INSANE.md](docs/ROADMAP_INSANE.md) | 五期填补计划（F/U/D/R/E 唯一任务编号） |
| [TASKBOARD.md](docs/TASKBOARD.md) | 可勾选任务表 |
| [PHILOSOPHY.md](docs/PHILOSOPHY.md) | 疯圣典：矛盾美学与「疯但好使」物理学 |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | 架构设计 + 非目标章 |
| [BOOT_SEQUENCE.md](docs/BOOT_SEQUENCE.md) | B0-B4 五阶段启动序列（演示版） |
| [ROADMAP.md](docs/ROADMAP.md) | Phase 0-9 历史存档 + 用户态主线 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | 开发环境、代码规范、Git 工作流 |
| [CLAUDE.md](CLAUDE.md) | 项目当前状态与下一阶段路线 |
| [LESSONS_LEARNED.md](LESSONS_LEARNED.md) | 开发经验与教训 |

## 构建

> ⚠ 当前构建仅支持 **Windows**（build.ps1 + WSL mkfs.vfat/mkfs.ext4），跨平台构建见 ROADMAP_INSANE F3。

```powershell
# 需求：LLVM Clang + ld.lld + Python/Pillow + QEMU + WSL(mkfs)
.\build.bat          # 构建 utsm.elf + deshab.elf + 14 个 .drv + ISO/deshab.img
.\build.bat -Variant dev      # 开发者模式（自动跑 shell 测试，调试全开）
.\build.bat -Variant release  # 发布版（进桌面+登录）
.\ISO\run_qemu.bat   # QEMU UEFI 启动验证（串口输出到 stdio）
```

---

> "Deshab 不假装安全，只声明边界；不假装完成，只标⬜。" 🪷
