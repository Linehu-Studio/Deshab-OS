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
| 页表映射接口（map/unmap） | ✅ | `kernel_api.mm_map_mmio` / `mm_unmap_mmio`（PCD\|PWT 独立窗口）；apic / ehci / xhci / nvme 四个驱动在用，含 BAR>4G 高位映射 | 串口 `BAR mapped via mm_map_mmio window` |
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
| ELF64 .drv 加载（4 stage） | ✅ | 17/17 驱动编译并装载 + driver_init（console_fb/timer/apic/acpi/pci/ahci/nvme/ehci/xhci/bootfs/vfs/fat32/devfs/e1000/virtio_net/ath9k/ps2kbd），与 manifest.json 一致 | QEMU 启动日志 `[<drv>] driver ready` ×14（ath9k/console_fb 无对应设备时走各自路径） |
| 驱动列表（timer/acpi/pci/ahci/nvme/bootfs/vfs/devfs/fat32/ps2kbd/e1000/virtio_net/console_fb/apic） | ✅ | 能力各异，见下行分项 | manifest.json |
| AHCI | ✅ | IDENTIFY/READ/WRITE DMA + ahci0 provider | — |
| NVMe | ✅ | admin 队列 + IDENTIFY + IO 队列(qid=1) + 读写/flush + PRP 跨页 + MSI/MSI-X（vector allocator 0x40–0xDF 池）+ block provider；NVME-E01..E03 零降级插桩。无控制器时 WARN 后正常 ready（设计内回退） | `CODE/DKM/nvme/nvme.c`；未挂盘时串口 `[nvme] NVMe controller not found` → `driver ready` |
| FAT32（DKM 侧） | 🔶 | 只读 BPB/FAT/目录项解析 + 根目录遍历；**无文件系统 API 层**（无 open/read/stat），当前仅 demo 级读取。大盘读写路径另由 DSK 侧 FAT32 实现承担（见下行 FS-E 表） | `CODE/DKM/fat32/fat32.c`；串口 `[fat32] driver ready` |
| e1000 | ✅ | RX/TX + DHCP/ARP/UDP/DNS（polling） | — |
| virtio_net | ✅ | legacy split-ring virtqueue（desc/avail/used）+ vq_setup + 32×2048B RX 缓冲 + TX/RX 环 + DHCP DISCOVER→OFFER selftest；VNET-E01/E02 零降级插桩。无设备时 WARN 后正常 ready（设计内回退） | `CODE/DKM/virtio_net/virtio_net.c`；未挂设备时串口 `[virtio_net] virtio-net device not found` → `driver ready` |
| 热卸载（D1） | ⬜ | 仅有 NO_UNLOAD 标志与状态机文档 | — |
| 按名 API 导出表（D3） | ✅ | `dkm_kapi_table`（magic/version/count/entries/lookup）挂在 `kernel_api->kapi` 尾部（ABI 兼容追加）；14 条按名导出（log.*/dma.alloc_pages/irq.register/irq.vector_*/mm.map_mmio/mm.unmap_mmio/apic.register_eoi/drr.*/utrw.debug_dump）；selftest 含正例调用 + 未命中负例 + TST-E21..E24 插桩 | `CODE/UTSM/dkm/kernel_api.c` + `test/utsm_selftest.c`（需 FUCK `[boot] selftest=2` 才执行该段） |
| 彩色日志（D4） | ✅ | `dkm_log_set_driver`（驱动名 FNV hash → 8 色调色板，确定可复现）；init 期间驱动日志带 ANSI 色（仅串口，BOOTLOG 纯文本）；FUCK `[debug] color_log` 总开关 | `.build_tmp/qemu_serial_d4color.log`（1150 彩色行） |

## DSK 主内核 / 用户态

| 子系统 | 状态 | 边界/说明 | 证据 |
|--------|------|----------|------|
| 启动 Logo（静态莲花） | ✅ | 旋转加载环已移除 | QEMU 现象 |
| FirstInit 向导 | ✅ | 输入+SHA256+写盘闭环；**联网不是进系统的前置条件**：检测到无无线设备直接跳过网络页，有设备时提供 Skip 按钮（渲染+鼠标+键盘三路），Connect 仍为零降级 NET-E01 panic | `.build_tmp/d2.log`（`no wireless device; skipping network setup page` → `USER.CONF` → login → desktop）|
| 可跳过 FirstInit（F5） | ✅ | `[dsk] skip_firstinit=1` 直达 desktop，跳过向导与登录 | `.build_tmp/qemu_serial_f1f4f5.log` |
| mouseInit（PS/2 鼠标） | ✅ | IRQ12 + 包解码 + 光标（M1） | `.build_tmp/qemu_serial_mouse.log` |
| desktop.elf | ✅ | 三页面+窗口+任务栏+双缓冲；打字机效果已删（整屏一次绘制）；DSK-E01 fatal 插桩 | — |
| SATA 边车镜像 | ✅ | `make_sata_sidecar.sh` 每次构建自动重建（旧 mkfat32.ps1 缺失导致 login.elf 长期缺盘的静默降级已根治） | build.ps1 |
| shell.elf | ✅ | 25+ 命令（ls/cat/cp/mv/run/probe/test/pci…） | — |
| login.elf | ✅ | USER.CONF + skip_login；ohMyLogo 开场屏 + 严格错误插桩（LOGIN-E01..E05 fatal，零降级） | `.build_tmp/login4.png` + qemu_serial_login4.log |
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

### FS-E 系列：文件系统读写严格模式（2026-10-02）

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| ext4 读写驱动（CODE/dsk/ext4.c） | ✅ | GPT 定位 p2 + feature 校验 + extent 树(depth0/1)读写 + 块/inode 分配回收 + 目录项插入 + 写后回读校验；启动自检 PASS | `.build_tmp/qemu_run.log`（[EXT4] selftest PASS） |
| FAT32 大盘写修复 | ✅ | 按需 FAT 表项 RMW（fat32_fat_read/write_entry）+ 块状空闲簇扫描（fat32_find_free_cluster）；修复 rec_len +4 收缩偏移与 chunk 对齐两处错位；**根目录链满时自动扩展**（分配新簇挂链尾后写条目，不再直接 `-4` no free dir entry）| 同上（USER.CONF verify ok → firstInit.txt flip ok）|
| ESP 簇大小（打包侧） | ✅ | `-s 4`（2KiB 簇 / 64 目录项）固定：避开 `spc=8` 的 65535 簇 OVMF 拒绝边界，也避开 `spc=16` 的 FAT32 下限，同时给 ESP 根目录留出 64 项余量（`spc=1` 只有 16 项，打包内容正好塞满导致首次启动写 USER.CONF 必失败）| `CODE/linux/pack_system_image.sh`；`.build_tmp/d2.log` |
| 中文字体位图字库（.dbf） | ✅ | `build.ps1` 调用 `CODE/font/mkfont.py` 由 `simhei.ttf` 生成 `simhei_16.dbf` / `simhei_24.dbf`（GB2312+ASCII）；此前无人生成，桌面必 panic DESK-E04 FONT PACK MISSING | `.build_tmp/d2.log`（`[zhfont] dbf loaded (16px ok)` → `desktop ready`）|
| FS-E01..E07 / FS-E2x / FS-E3x 插桩 | ✅ | 写失败/校验不符/feature 不支持/路径缺失 → 莲花 panic 零降级 | `.build_tmp/qemu_run.log`（FS-E02/E06/E22 复现） |
| 向导后强制重登 | ✅ | first-boot 分支 FirstInit→persist→login；登录跳过/失败 → FS-E06 panic | 同上（login success → desktop ready） |
| NET-E01 网络页 Connect panic | ✅ | 无 wlan 栈，Connect 即 panic；Save(仅存配置) 为唯一前进路径 | 同上（NET-E01 复现，fb pmemsave 确认莲花屏） |
| 主镜像改挂 SATA（run_qemu.bat） | ✅ | DSK/AHCI 块层必须可见 GPT 盘才能访问 p2 ext4；边车退居 ide.1 | run_qemu.bat |
| 开机画面去 Logo | ✅ | 移除 Logo.png 图像，仅保留居中 "DESHAB P01" 文字；dcp/xj logo 改由 TEXTURES/ 重新生成 | build 日志 |

## DSK-E70：ibuf 尾部写零破坏（2026-10-05 · 未解）

| 项 | 状态 | 说明 | 证据 |
|----|------|------|------|
| 现象 | 🔶 | DSK 经 `dsk_load_elf` 载入 login.elf 并跳转后，image 尾部（ibuf+0x1EAE10 附近，≥ 数百字节）被清零。`.text` ≤ ~16.5KB 时雷区覆盖 .got/.bss 关键数据 → login integrity 校验 len 计算错误 → LOGIN-E01 误报；`.text` ≥ ~17KB 时雷区落在 .rodata 无害 | `.build_tmp/qemu_dbg.log`（boot 矩阵：16248B FAIL×4 / 16504B FAIL×2 / 17352、17608B OK；DSK 侧 checksum 跳转前位精确 + QMP 物理内存 dump 停机后 .text 位精确与 got[0]=0 并存） |
| login 侧加固 | ✅ | ① `__text_start/end/__expected_hash` 加 hidden 可见性 → PIE 下 lea rip 取址，len 计算零 GOT 依赖；② `dsk_e70_layout_guard` 显式将 .text 推入安全布局（白名单登记，根因修复后删除） | CODE/login/main.c |
| patch 侧加固 | ✅ | patch_login_hash.ps1 改按 PT_LOAD filesz 计算 hash 范围（与 DSK loader 加载范围/运行时 [__text_start,__text_end) 严格一致），并用 e_shstrndx 定位 .shstrtab | CODE/login/patch_login_hash.ps1 |
| 根因（写零者） | ⬜ | 嫌疑面：AHCI DMA PRDT 边界 / fat32_read_sectors 缓冲边界 / DSK .bss 内大缓冲（g_fdata 10MB、ibuf 8MB）相邻越界。写零发生在跳转后、sha256 前；确切写者与范围待断点级排查 | — |

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
