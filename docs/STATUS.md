# Deshab-OS 状态总表（单一事实源）

> 本文件是全项目唯一的子系统状态来源。其他文档（README/ROADMAP/ARCHITECTURE/CLAUDE.md）只引用本表，不得自抄状态。
> 状态规范：✅ 已实现 · 🔶 演示级（注明边界） · ⬜ 仅设计/未实现。
> 更新规则：状态变更必须同步本表，并附证据（串口日志路径 / QEMU 现象）。

## 内核底座

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 早期启动（Limine→long mode→C） | ✅ | cli/cld 纪律、early serial 非阻塞 | QEMU 启动日志 |
| IDT/异常 | ✅ | 0–255 stub 全覆盖；异常路径走 F1 莲花崩溃屏 | `arch/x86_64/idt.c` |
| PIC remap + IRQ 分发 | ✅ | remap 0x20–0x2f，handler 注册 + EOI | — |
| IOAPIC→LAPIC 路由（B7） | ✅ | apic_route=1 接管；失败自动降级 PIC | `.build_tmp/qemu_serial_*.log` |
| LAPIC timer | ✅ | TSC 校准 + 周期模式，[sched] 配置 | M4 QEMU 日志 |
| DMA 物理页分配器 | ✅ | bitmap，低 4G 钳位 | e1000 DMA 根治记录 |
| 页表映射接口（map/unmap） | ⬜ | mmio 通用映射待做 | — |
| panic 统一兜底（F1） | ✅ | DEAICUP 莲花崩溃屏（ohMyLogo.png 内嵌，背景=图片背景色）+ panic_symbol + DRR 归档；CPU 异常/驱动 panic 统一入口 | `.build_tmp/panic_screen.png` + qemu_serial_f1logo2.log |
| boot 分级自检（F4） | ✅ | `[boot] selftest=0\|1\|2`，5 项全过 BOOT-OK | `.build_tmp/qemu_serial_f1f4f5.log` |
| 帧缓冲控制台抽象 fbcon（F2） | 🔶 | `include/utsm/fbcon.h`：像素/混合/填充/RGBA 图像/文本（panic 路径已用）；统一控制台后端 ⬜ | `CODE/UTSM/include/utsm/fbcon.h` |
| 构建系统 | ✅ | Windows build.ps1 + 跨平台根 Makefile（Linux/macOS `make dev/release/realtest`，打包走 pack_system_image.sh）；macOS 缺 mkfs.ext4 时打包不可用 | `Makefile` |

## UTSM / UTRW

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 封缄段表 / UUID 调谐（U1） | ✅ | 段创建/查询闭环 | `CODE/UTSM/core/segment.c` |
| UTRW Fast Path | ✅ | capability→PCKC→line 解密→seqlock | `utrw/read.c` |
| UTRW Slow Path（U2） | ✅ | EPOCH 刷新 / KEY_MISS 重派生 / POISONED+MAC_FAILED→DRR / STALE_CAP 拒绝；全分支 selftest PASS | `utrw/slow_path.c` + `.build_tmp/qemu_serial_u2slowpath.log` |
| `@sealed` 段标记（U3） | ✅ | 默认封缄；`UTSM_SEG_F_UNSEALED` 段明文直存（0 加解密开销，capability/dirty/checkpoint 照走）；linker.ld `.sealed` section + `__sealed_start/end` 符号就位 | `.build_tmp/qemu_serial_u3sealed.log` + llvm-nm |
| 密文页 dump（U4） | ✅ | `utrw_debug_dump_page(slot,page,offset)`：串口输出 64B 密文/明文 hex 对照（selftest 内置演示） | `.build_tmp/qemu_serial_u4dump.log` |
| selftest（U5） | ✅ | chacha20 KAT 落地为 RFC 8439 §2.4.2 官方向量（gen_chacha_kat.py 生成，pycryptodome 独立对拍）；"间歇 FAIL" 确认为幽灵缺陷（该测试从未合并进本树）；page MAC 确定性检查已加；全量 selftest 稳定 PASS | `.build_tmp/qemu_serial_u5kat.log` |
| 真实加密原语 | 🔶 | chacha20/KDF/MAC 已落（Phase 9 早期），PCKC schedule 待接 | `crypto/` |

## DKM 驱动系统

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| ELF64 .drv 加载（4 stage） | ✅ | 14/14 驱动装载 + driver_init | QEMU 启动日志 |
| 驱动列表（timer/acpi/pci/ahci/nvme/bootfs/vfs/devfs/fat32/ps2kbd/e1000/virtio_net/console_fb/apic） | ✅ | 能力各异，见下行分项 | manifest.json |
| AHCI | ✅ | IDENTIFY/READ/WRITE DMA + ahci0 provider | — |
| NVMe | 🔶 | PCI discovery only，BAR0>4G 待映射 | — |
| FAT32（DKM 侧） | 🔶 | 只读，子目录路径待完善 | — |
| e1000 | ✅ | RX/TX + DHCP/ARP/UDP/DNS（polling） | — |
| virtio_net | 🔶 | PCI/capability 枚举，无 virtqueue | — |
| 热卸载（D1） | ⬜ | 仅有 NO_UNLOAD 标志与状态机文档 | — |
| 按名 API 导出表（D3） | ⬜ | 固定偏移 struct ABI | `CODE/sdk/include/deshab/kernel_api.h` |
| 彩色日志（D4） | ⬜ | 仅 info/warn/error/panic | — |

## DSK 主内核 / 用户态

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 启动 Logo（静态莲花） | ✅ | 旋转加载环已移除 | QEMU 现象 |
| FirstInit 向导 | 🔶 | 输入+SHA256+写盘闭环；按键崩溃未复现（B6） | `.build_tmp/qemu_serial_b6.log` |
| 可跳过 FirstInit（F5） | ✅ | `[dsk] skip_firstinit=1` 直达 desktop，跳过向导与登录 | `.build_tmp/qemu_serial_f1f4f5.log` |
| mouseInit（PS/2 鼠标） | ✅ | IRQ12 + 包解码 + 光标（M1） | `.build_tmp/qemu_serial_mouse.log` |
| desktop.elf | ✅ | 三页面+窗口+任务栏+双缓冲 | — |
| shell.elf | ✅ | 25+ 命令（ls/cat/cp/mv/run/probe/test/pci…） | — |
| login.elf | ✅ | USER.CONF + skip_login | — |
| PE/EXE 兼容层 | ✅ | PE32+ 原生 / PE32 走 x86emu32 | — |
| Linux 兼容层 | 🔶 | VMM+park-and-resume；VMX 实机验证待 VT-x/KVM | — |
| Pacman | ⬜ | 五阶段方案 | — |

## 调度 / 恢复

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| SAS-R0-PCQ 调度器 | 🔶 | O(1) 位图 + LAPIC tick + xv6 式切换 ✅；BSP-only，per-cpu 多核 ⬜ | `.build_tmp/qemu_serial_sched_m4.log` |
| task_kill crypto erase | ⬜ | key_epoch++ 待做 | — |
| DRR checkpoint（R1） | ✅ | dirty shard → dma 页快照 + A/B 双槽 + CRC64 原子切换 | M4 QEMU 日志 |
| DRR 回滚（R4） | 🔶 | page 级 + BLAKE2b keyed 页 MAC ✅；segment/system 级待验证 | M4 QEMU 日志 |
| DRR 看门狗（R2） | ✅ | timeout 检出→回滚/复位；负向测试 PASS | M4 QEMU 日志 |
| Emergency Pool（R3） | ✅ | 64KB 编译期独立池；`drr_emergency_pool_kb`/`drr_recovery_log_size` FUCK 接线 ✅（运行期钳位） | `.build_tmp/qemu_serial_r3b.log` |

## 文档体系

| 文档 | 状态 | 说明 |
|------|------|------|
| [STATUS.md](./STATUS.md) | ✅ | 单一状态源（本文件） |
| [ROADMAP_INSANE.md](./ROADMAP_INSANE.md) | ✅ | 五期填补计划（唯一任务编号 F/U/D/R/E） |
| [TASKBOARD.md](./TASKBOARD.md) | ✅ | 可勾选任务表 |
| [PHILOSOPHY.md](./PHILOSOPHY.md) | ✅ | 疯圣典 |
| [ROADMAP.md](./ROADMAP.md) | ✅ | Phase 0–9 历史存档 + 用户态主线 |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | ✅ | 架构 + 非目标章 |
| [BOOT_SEQUENCE.md](./BOOT_SEQUENCE.md) | ✅ | B0–B4 演示版 |
| ~~系统开发策划.md~~ | 已删除 | 状态与本表矛盾，收编于此 |
