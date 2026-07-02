# Deshab-OS

Deshab-OS 是一个从零编写的独立 64 位操作系统，基于 x86_64 long mode，采用单地址空间 Ring0（SAS-R0）内核架构，配套封缄内存（UTSM）、专用恢复根（DRR）与可加载驱动模块系统（DKM）。

## 项目状态

当前已完成可启动的实验内核闭环：Limine 引导 → UTSM 首阶段内核 → 14 个 DKM 驱动加载 → DSK 主内核 → 首次启动用户设置向导。

| 子系统 | 状态 |
|--------|------|
| 早期启动 / IDT / PIC | ✅ |
| UTSM 封缄内存（selftest） | 🔶 |
| DKM 驱动系统（14/14 驱动） | ✅ |
| DMA 物理页分配器 | ✅ |
| AHCI / NVMe 块设备 | 🔶 |
| FAT32 只读文件系统 | 🔶 |
| e1000 RX/TX 网络 | 🔶 |
| 字体系统（DBF，GB2312 全覆盖） | ✅ |
| FirstInit 用户设置向导 | 🔶 |
| SAS-R0-PCQ 调度器 | ⬜ |
| DRR 恢复系统 | ⬜ |

## 核心架构

```text
SAS-R0-PCQ  单地址空间 Ring0 + Per-CPU O(1) 位图调度器
UTSM        UUID 调谐封缄内存（所有内存存密文，明文须经 UTRW 读写器）
UTRW        带标记读写器（Fast Path / Slow Path）
DRR         专用恢复根（看门狗 / checkpoint / A-B 回滚 / 独立 Emergency Pool）
DKM         驱动模块系统（ELF64 .drv ABI，4 stage 分阶段加载）
```

## 文档

| 文档 | 内容 |
|------|------|
| [架构设计文档](docs/ARCHITECTURE.md) | 系统整体架构、五大核心子系统、内存模型、驱动系统 |
| [启动路线设计](docs/BOOT_SEQUENCE.md) | B0-B4 五阶段启动序列、启动契约、回退链、视觉体验 |
| [开发路线图](docs/ROADMAP.md) | 分阶段开发路线、里程碑、验收标准 |
| [贡献与协作规范](CONTRIBUTING.md) | 开发环境、代码规范、Git 工作流、PR 流程 |
| [CLAUDE.md](CLAUDE.md) | 项目当前状态与下一阶段路线 |
| [UTSM 模块设计](CODE/UTSM/README.md) | 封缄内存模块完整设计 |
| [DKM 驱动系统](CODE/DKM/README.md) | 驱动模块系统说明 |
| [DSK 主内核](CODE/dsk/README) | 主内核说明与 boot context |
| [开发经验记录](LESSONS_LEARNED.md) | 开发过程中的经验与教训 |

## 构建

```powershell
# 需求：LLVM Clang + ld.lld + Python/Pillow + QEMU
.\build.bat          # 构建 utsm.elf + deshab.elf + 14 个 .drv + ISO/deshab.img
.\ISO\run_qemu.bat   # QEMU UEFI 启动验证
```
