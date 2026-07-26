# Deshab 项目上下文

## 项目概览

Deshab 当前包含一个内核/系统实验方向，其中启动内核与主内核目标为：

```text
CODE/UTSM -> SYSTEM/boot/utsm.elf
CODE/dsk  -> SYSTEM/system/deshab64/deshab.elf
```

`utsm.elf` 是当前由 bootloader 直接加载的首阶段内核，负责 UTSM、DKM 驱动加载、平台发现和早期系统服务。当前已实现两阶段 DSK 加载：优先通过 `kernel_api.block` + 内嵌 FAT32 解析从真实块设备读取 `/system/deshab64/deshab.elf`（在 SATA FAT32 盘场景已验证）；无块设备时自动回退 Limine boot module 预加载。

DSK 主内核（`deshab.elf`）当前已实现：
- 内嵌 FAT32 只读解析器（256 扇区批量读取 + 内存内文件查找）
- PIE ELF loader（PT_LOAD + PT_DYNAMIC + R_X86_64_RELATIVE 重定位）
- 静态启动 Logo（嵌入 Logo.png RGBA 数据，居中 alpha 混合绘制于渐变背景上，取代原旋转加载动画）
- 首次启动检测：读取 `firstInit.txt`，值为 `0` 则触发首次启动流程
- 系统初始化调度：DSK 直接加载并调用 `mouseInit.elf`（PS/2 鼠标安全初始化）
- 用户设置向导：DSK 加载并跳转 `FirstInit.elf`（圆角卡片 UI、键盘输入、SHA256 密码、加密配置缓冲）

FirstInit（`CODE/firstInit`）只负责用户级设置向导：
- 淡蓝背景 → "欢迎使用 Deshab"（中英双语，英文小字副标题）→ 设置提示 → 账户设置卡片
- PS/2 键盘轮询输入：计算机名、用户名、密码
- SHA256 密码摘要 + XOR 加密配置缓冲（user.conf 格式已定义，写盘待 FAT32 write API）
- 18px 黑体中文位图（预渲染嵌入 ELF）

mouseInit（`CODE/mouse`）是 DSK 直接调度的系统输入初始化器：
- 当前为安全 stub（只读 PS/2 状态，不写控制器），避免阻塞首次启动
- 后续将实现完整 IRQ12 + 鼠标包解码

## 当前核心架构

```text
SAS-R0-PCQ
    单地址空间 Ring0 任务模型
    Per-CPU O(1) 位图调度器

DRR
    Dedicated Recovery Root
    看门狗 / checkpoint / recovery log / A-B 回滚

UTSM
    UUID-Tuned Sealed Memory
    UUID 调谐封缄内存

UTRW
    UTSM Tagged Reader/Writer
    UTSM 带标记读写器

DMP
    Descriptor Meta Page
    段描述符元页

PCKC
    Per-CPU Key Cache
    每 CPU 密钥缓存
```

## 关键设计约束

1. 优先考虑极高权限和极高效率。
2. 主系统采用单地址空间 Ring0 模型。
3. 不追求传统多用户强安全隔离。
4. DRR 作为恢复根，应尽量独立于普通调度器。
5. UTSM 数据区连续存密文，UUID 不嵌入数据区。
6. 调度器热路径必须保持 O(1)。
7. checkpoint 只处理 dirty shard / dirty page，不扫描全内存。
8. DRR 必须拥有独立 Emergency Pool，普通 OOM 不应阻断 recovery。
9. checkpoint metadata 使用 A/B 双槽、CRC 校验和原子 active slot 切换。
10. 驱动模块采用 DKM ELF64 .drv ABI，通过 driver_desc / driver_init / driver_exit 暴露入口。
11. 驱动默认通过 dkm_kernel_api 调用内核服务，不直接依赖任意内核符号。
12. stage0/stage1 驱动必须支持 bootloader 预加载 boot module 方案。

## 重要文件

```text
SYSTEM/
    打包为 IMG 后的系统根目录。

SYSTEM/driver/manifest.json
    内核启动驱动模块清单，控制 ELF .drv 驱动的分阶段加载。

CODE/DKM/README.md
    DKM 驱动模块系统说明，记录驱动加载目标、ABI、loader、状态机和实现清单。

CODE/UTSM/README.md
    UTSM 模块设计说明。

RE/UTSM_算法记录.md
    UTSM 核心算法记录。

RE/UTSM_设计架构.md
    UTSM 架构记录。

RE/驱动模块ABI设计.md
    DKM 驱动模块 ABI、kernel_api、ELF relocation、manifest、状态机和 boot module 方案。

ISO/deshab.img
    build.ps1 将 SYSTEM 打包生成的 GPT 磁盘镜像，包含一个 FAT32 EFI System Partition。

ISO/run_qemu.bat
    使用 QEMU 启动 ISO/deshab.img 的脚本。
```

## 构建相关

已知说明：

```text
SYSTEM/ 是打包为 IMG 后的系统根目录。
SYSTEM/boot/ 存放启动模块，例如 utsm.elf。
SYSTEM/driver/ 存放 ELF .drv 驱动模块和 manifest.json。
CODE/UTSM/README.md: 编译到 SYSTEM/boot/utsm.elf
build.bat: 调用 build.ps1。
build.ps1: 编译 UTSM，生成 SYSTEM/boot/utsm.elf，并把 SYSTEM 打包为 ISO/deshab.img。deshab.img 是 GPT 磁盘镜像，包含 FAT32 EFI System Partition。
ISO/run_qemu.bat: 用 QEMU + UEFI 固件启动 ISO/deshab.img，串口输出到 stdio。
```

当前 UTSM 已有首阶段内核代码骨架和构建脚本，可生成 utsm.elf 与 GPT + FAT32 ESP 镜像。早期启动要求：进入 C 前 `cli/cld`，early serial 不允许无限等待硬件 ready 位，未启用 FPU/SSE 前编译必须使用 `-mno-sse -mno-sse2 -mno-mmx -msoft-float`。DATA/BSS 段使用 PF_R|PF_W|PF_X（SAS-R0 下驱动模块内存需可执行）。

DKM/DSM 驱动加载系统已完工：内置 `console_early` + manifest.json 解析 + Limine boot module 预加载 + 4 stage 分阶段加载 + 全部 14 个外部 `.drv` ET_REL 装载（ELF64/SHT_NOBITS/R_X86_64_64/32/32S/PC32 relocation + driver_desc 校验 + driver_init 调用）。当前镜像额外预加载 `driver/test.fat32` 作为 FAT32 驱动测试镜像。

当前已实现 14 个真实 DKM 驱动：
- **timer** (stage0): PIT 校准, 100ms busy-wait demo
- **apic** (stage0): CPUID Local APIC 探测, IA32_APIC_BASE MSR, ACPI MADT 枚举, LAPIC MMIO 只读寄存器验证；当前保留 PIC IRQ 路由
- **acpi** (stage0): Limine RSDP → XSDT, ACPI 表枚举
- **pci** (stage0): PCI config space 扫描, 设备枚举
- **console_fb** (stage1): Limine framebuffer, 淡蓝色背景清屏（原四色圆弧加载环已移除，启动画面改由 DSK 静态 Logo 接管）
- **ahci** (stage1): PCI AHCI `8086:2922` 探测, BAR5/ABAR HHDM MMIO 映射, HBA CAP/GHC/PI/VS 与 port 枚举；已接入 `kernel_api.dma`，支持 SATA disk IDENTIFY 与 LBA0 READ 最小 DMA 路径；可向 `kernel_api.block` 注册 `ahci0` block provider；默认无 SATA 盘/ATAPI 场景会跳过命令
- **nvme** (stage1): PCI NVMe `1b36:0010` 探测, BAR0/BAR1 解析, IRQ line 读取；当前 QEMU BAR0 位于 4G 以上，MMIO register 读取等待高位 PCI MMIO 映射能力
- **bootfs** (stage1): Limine boot module 内存文件系统
- **vfs** (stage2): 挂载 bootfs, 文件查找/读取
- **devfs** (stage2): 暴露 `/dev/version`, `/dev/platform`, `/dev/fb0`, `/dev/modules`, `/dev/boot/`
- **fat32** (stage2): 只读 FAT32 BPB/FAT/目录项解析, 通过 `driver/test.fat32` 验证读取 `README.TXT`
- **ps2kbd** (stage3): PS/2 IRQ1 注册, 端口 `0x60` 扫描码读取, scan code set 1 → ASCII demo
- **e1000** (stage3): PCI `8086:10d3` 探测, BAR0 HHDM MMIO 映射, MAC 读取, IRQ 注册, 基础 link setup
- **virtio_net** (stage3): PCI virtio-net `1af4:1001` 探测, BAR 枚举, modern virtio PCI capability 枚举；暂不进行 feature negotiation / virtqueue / 收发包

当前内核平台能力：IDT 0–47 stub、异常处理、PIC remap 到 `0x20–0x2f`、IRQ handler 注册与 EOI、Limine RSDP/framebuffer/module/HHDM request。

kernel_api 已暴露能力：log, rsdp_address, fb_address/width/height/pitch/bpp, boot_modules_response, irq_register, hhdm_offset, dma.alloc_pages。

当前 manifest 中 14 个外部 `.drv` 均已替换为真实 DKM 驱动并可加载执行。

下一阶段优先路线：
1. **内存与 MMIO 基础设施**: 实现物理页分配器、页表映射接口、低位/高位 PCI MMIO 映射，解决 NVMe 4G 以上 BAR 和后续 DMA 映射问题。
2. **DMA 与块设备数据路径**: 建立 contiguous DMA buffer、cache/屏障约定、PRDT/队列内存管理，然后推进 AHCI IDENTIFY/READ 与 NVMe admin queue/identify。
3. **IRQ 后端升级**: 在现有 PIC fallback 稳定基础上扩展 IDT/vector allocator/APIC EOI/IOAPIC redirection，逐步迁移设备 IRQ，最后接入 MSI/MSI-X。
4. **网络数据路径**: 在 DMA/IRQ 完成后推进 e1000 RX/TX ring 与 virtio-net feature negotiation/virtqueue/RX-TX。
5. **VFS 与真实块设备接入**: 将 FAT32 从测试镜像迁移到 AHCI/NVMe block provider，完善挂载、读取、目录遍历和错误路径。
6. **DKM 工程化收尾**: 统一驱动本地 ABI 头、清理 warning、补充自动化 QEMU 场景（e1000/virtio-net/nvme/ahci）。
7. **FirstInit 完善**: 修复按键崩溃（fb_text 优化问题）、实现鼠标光标移动、user.conf 写盘（需 block write API）、网络配置界面。
8. **鼠标输入驱动**: 从 mouseInit stub 升级为完整 PS/2 鼠标 IRQ12 + 包解码 + 光标移动。
