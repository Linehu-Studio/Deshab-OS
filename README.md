# Deshab-OS

> 实验内核 · QEMU 可启动演示 · 非生产系统

Deshab-OS 是一个从零编写的独立 64 位操作系统，基于 x86_64 long mode，采用单地址空间 Ring0（SAS-R0）内核架构，配套封缄内存（UTSM）、专用恢复根（DRR）与可加载驱动模块系统（DKM）。

当前已完成可启动的实验内核闭环：Limine 引导 → UTSM 首阶段内核 → 14 个 DKM 驱动加载 → DSK 主内核 → 桌面/登录/向导。

## 项目特质

Deshab 的灵魂是**自相矛盾**：一边封圣一边爆粗，一边敞开门一边焊死锁——**疯的是姿态，稳的是时序**。

1. **疯但好使。** 提交历史像涂鸦墙，但 `run_qemu.bat` 一下，莲花确实开了。每个里程碑必须有 QEMU 证据，疯要有画面证据。
2. **毛坯神殿，不是赛博保险柜。** SAS-R0 单地址空间 Ring0——没有隔断的机房，每面墙上写着"此处无墙"。自由是默认值：用户代码即内核代码，想改就改；卡了按时光键（DRR 回滚）。
3. **仪式感是功能。** 给空气上锁（UTSM 封缄）不是因为有人窥探，是因为喜欢咔哒声——所以它必须是可开关的旋钮（`@sealed` 可选标记），而不是强加的道袍。崩溃也要崩出莲花（DEAICUP 莲花崩溃屏）。
4. **诚实比体面重要。** 不假装安全，只声明边界；不假装完成，只标⬜。状态唯一来源是 [STATUS.md](docs/STATUS.md)，每个 ✅ 都带证据路径，⬜ 是坦白不是路线装饰。
5. **错也错得有章法。** 0bug 不靠形式化验证，靠 B0-B4 启动契约、分级自检（F4）、看门狗与 A/B checkpoint 回滚兜底。

完整宣言与三条箴言见 [docs/PHILOSOPHY.md](docs/PHILOSOPHY.md)。

## 核心架构

```text
SAS-R0-PCQ  单地址空间 Ring0 + Per-CPU O(1) 位图调度器（当前 BSP-only）
UTSM        UUID 调谐封缄内存（明文须经 UTRW 读写器）
UTRW        带标记读写器（Fast/Slow Path ✅，RFC 8439 KAT 对拍）
DRR         专用恢复根（看门狗 / checkpoint / A-B 回滚 / 独立 Emergency Pool）
DKM         驱动模块系统（ELF64 .drv ABI，4 stage 分阶段加载）
```

## 项目状态

**唯一状态源见 [docs/STATUS.md](docs/STATUS.md)**，本表只是引用摘要。

| 子系统 | 状态 |
|--------|------|
| 早期启动 / IRQ / DMA / DKM 14 驱动 | ✅ |
| UTSM 封缄（Fast/Slow Path + RFC 8439 KAT + @sealed 旋钮） | ✅ |
| AHCI / e1000 / FAT32 写盘 | ✅ |
| NVMe / virtio-net / FAT32 完整路径 | 🔶 |
| PCQ 调度器 / DRR 恢复根 | ✅（BSP-only / 演示级） |
| 五期填补计划进度 | 一期二期全勾，见 [docs/TASKBOARD.md](docs/TASKBOARD.md) |

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

> Windows 走 `build.bat`；Linux/macOS 用根目录 [Makefile](Makefile)（`make dev|release|realtest`，打包需 mkfs.vfat/mkfs.ext4）。

```powershell
# Windows：LLVM Clang + ld.lld + Python/Pillow + QEMU + WSL(mkfs)
.\build.bat          # 构建 utsm.elf + deshab.elf + 14 个 .drv + ISO/deshab.img
.\build.bat -Variant dev      # 开发者模式（自动跑 shell 测试，调试全开）
.\build.bat -Variant release  # 发布版（进桌面+登录）
.\ISO\run_qemu.bat   # QEMU UEFI 启动验证（串口输出到 stdio）
```

```bash
# Linux / macOS（F3 跨平台构建）
make dev             # 等价 build.bat -Variant dev
make both            # dev + release
```

---

> "Deshab 不假装安全，只声明边界；不假装完成，只标⬜。" 🪷
