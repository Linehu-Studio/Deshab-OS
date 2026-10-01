# Deshab-OS 架构设计文档

> 系统整体架构、核心子系统与关键技术约束。模块文档索引：[UTSM](../CODE/UTSM/README.md)、[DKM](../CODE/DKM/README.md)、[DSK](../CODE/dsk/README)。

---

## 1. 项目定位

| 属性 | 说明 |
|------|------|
| 架构 | x86_64 long mode，Higher Half `0xffffffff80000000+` |
| 权限 | 单地址空间 Ring0（SAS-R0），无传统多用户隔离 |
| 引导 | Limine（UEFI 优先，BIOS 兼容） |
| 语言 | C11 freestanding + x86_64 asm |
| 工链 | LLVM Clang + ld.lld（`-mcmodel=kernel -mno-sse -msoft-float`） |
| 构建 | `build.ps1` → `ISO/deshab.img`（GPT：FAT32 ESP `/boot` + ext4 数据分区）→ QEMU UEFI |

设计哲学：极高权限/效率优先（Ring0+O(1)热路径）；内存默认封缄（UTSM/UTRW）；恢复根独立（DRR Emergency Pool）；驱动与内核解耦（dkm_kernel_api）；先闭环再扩展。

---

## 2. 系统整体架构

### 2.1 启动链路

```text
UEFI/BIOS → Limine → utsm.elf（首阶段：serial/IDT/arena/DMA/UTSM/DKM→14驱动/FUCK配置）
  → dsk_boot_context(RDI) → deshab.elf（DSK：首次启动/正常启动/FUCK配置）
```

### 2.2 双阶段内核

| 阶段 | 产物 | 职责 |
|------|------|------|
| UTSM | `SYSTEM/boot/utsm.elf` | Long mode入口/serial/IDT/arena/DMA/UTSM init/DKM驱动加载/平台发现/DSK加载跳转 |
| DSK | `SYSTEM/system/deshab64/deshab.elf` | 接收boot context/首次启动调度/块设备VFS/网络运行期 |

跳转约定：`RDI=dsk_boot_context*, RSI=RDX=0, IF=0, paging=UTSM页表`

### 2.3 UTSM 启动序列

```text
kernel_main() → serial_init → idt_init → arena_init(16MB) → dma_init(Limine USABLE→bitmap)
  → net_init(8槽) → drr_stub_init → utsm_init → dkm_init+fill_platform_info
  → dsm_load_by_manifest(4 stage, 14 .drv) → utsm_selftest_run → dsk_load_and_jump
```

---

## 3. 五大核心架构支柱

### 3.1 SAS-R0-PCQ — 单地址空间Ring0调度模型

- 单地址空间，Ring0，Per-CPU独立runqueue，调度热路径O(1)
- 任务结构携带UTSM加密上下文（process_uuid/capability table/hot_segment_hint）
- 任务状态：`READY→RUNNING→BLOCKED/SLEEPING→RECOVERING/FAULTED→ZOMBIE`
- **约束**：调度切换禁止扫描capability/计算MAC/写checkpoint/重加密

### 3.2 UTSM — UUID调谐封缄内存

所有普通内存存密文，明文读写须经UTRW。UUID 128-bit二进制格式。

核心规则：数据区只存连续密文（UUID不插入数据区）；每段独立DMP；每任务process_uuid+capability table；加密粒度64B cache line，Dirty粒度4KB page，MAC粒度4KB page。

```text
段布局：[DMP: Descriptor Meta Page][Cipher Data Page 0][Cipher Data Page 1]...
加密方案：
  segment_key = KDF(DRR_root_key, process_uuid, segment_uuid, key_epoch)
  tweak = hash(hash(process_uuid, segment_uuid, key_epoch), line_index, key_epoch)
  ciphertext = plaintext XOR stream(segment_key, tweak)
段状态：FREE→ACTIVE→CHECKPOINTING/SEALED→RECOVERING→POISONED/DESTROYED
```

### 3.3 UTRW — 带标记读写器

| 层级 | 说明 |
|------|------|
| L0 Raw | 直接读cipher_base（密文） |
| L1 Fast Path | capability O(1)定位→PCKC命中→无全局锁解密 |
| L2 Slow Path | key miss/epoch mismatch/MAC fail异常处理 |

Fast Read用seqlock（读writer_seq before→读数据→读after，不等则重试）。Fast Write：`writer_seq++`进奇数→按64B line加密→写密文→标dirty bitmap→`writer_seq++`回偶。部分写须先解密旧64B line再合并。

Slow Path：KEY_MISS→派生key写PCKC；EPOCH_EXPIRED→刷新cap.epoch；MAC_FAILED→标suspicious+DRR+试rollback；SEGMENT_POISONED→交DRR。

### 3.4 DRR — 专用恢复根

核心约束：独立Emergency Pool（普通OOM≠DRR OOM）；recovery路径禁止依赖普通堆；checkpoint只处理dirty shard/page不扫描全内存。

Checkpoint类型：Light（段表/DMP/dirty summary/writer_seq/key_epoch/MAC root）；Dirty Page（dirty页密文副本+MAC）；Boot（内核slot/UTSM root epoch/DRR boot status）。

A/B双槽：active=A→新checkpoint写B→metadata+dirty index+MAC root+CRC→flush→原子切active=B。恢复优先active slot，CRC错则fallback，两槽皆坏则system rollback。

回滚级别：Page→Segment→System。Dirty Shard优化：dirty bitmap拆shard，跳过dirty_count==0的shard，复杂度O(dirty_shards+dirty_pages)。

### 3.5 DKM — 驱动模块系统

分阶段加载（manifest.json）：

| Stage | 驱动 | 失败策略 |
|-------|------|---------|
| 0 platform | timer/apic/acpi/pci | required失败→DRR/panic |
| 1 boot | console_fb/ahci/nvme/bootfs | required失败→DRR recovery |
| 2 filesystem | vfs/fat32/devfs | 失败→fallback bootfs |
| 3 optional | e1000/virtio_net/ps2kbd | 失败标记FAILED继续 |

驱动ABI：导出`driver_desc`(magic=0x444B4D31)/`driver_init`/`driver_exit`。kernel_api服务：log/mem/utsm/irq/pci/dma/vfs/net/timer/drr + 平台直通(rsdp/fb_*/boot_modules/irq_register/hhdm) + block provider registry。

ELF Loader：必须R_X86_64_64/RELATIVE/GLOB_DAT/JUMP_SLOT，禁止TLS/IFUNC/lazy binding。加载状态机：DISCOVERED→QUEUED→DEP_WAIT→LOADING→ELF_CHECKED→MEMORY_ALLOCATED→RELOCATED→ABI_CHECKED→INITING→ACTIVE。

---

## 4. 辅助数据结构

### 4.1 DMP — 段描述符元页
热路径加速结构，O(1)段定位/边界检查/tweak生成/dirty bitmap定位。含fast_base/fast_limit/fast_tweak_seed/fast_key_epoch+CRC。

### 4.2 PCKC — 每CPU密钥缓存
默认8槽，缓存hot segment key schedule。调度切换只加载next.crypto指针+hot_segment_hint。替换LRU/最冷slot。

### 4.3 Capability — 段访问凭证
热路径`segment_slot+generation+epoch`直接定位验证。权限：READ/WRITE/EXEC/SHARE/DMA。默认cap[0]=stack(RW)/cap[1]=heap(RW)/cap[2]=ipc(RWS)/cap[3]=code(RX)。

---

## 5. 内存管理

| 组件 | 实现 | 位置 |
|------|------|------|
| 内核arena | 16MB bump | `CODE/UTSM/mm/arena.c` |
| DMA分配器 | Limine USABLE→bitmap，低4G钳位 | `CODE/UTSM/mm/dma.c` |
| UTSM段分配 | DMP+密文数据区 | `CODE/UTSM/core/segment.c` |

全局区域：Normal Encrypted Segment / DMP Pool / Segment Table / PCKC Area / DRR Reserved Region

关键约束：DMA缓冲须来自物理页分配（不能用高半虚拟地址反推物理）；32位BAR设备DMA需落低4G；DATA/BSS用PF_R|PF_W|PF_X；未启用SSE前-mno-sse -msoft-float。

---

## 6. 驱动子系统现状

| Stage | 驱动 | 实现 |
|-------|------|------|
| 0 | timer | PIT校准+100ms demo |
| 0 | apic | CPUID/MADT/LAPIC MMIO只读，保留PIC IRQ |
| 0 | acpi | RSDP→XSDT，表枚举 |
| 0 | pci | config space扫描枚举 |
| 1 | console_fb | Limine fb，淡蓝背景 |
| 1 | ahci | PCI探测+ABAR HHDM+IDENTIFY/READ/WRITE DMA+ahci0 provider |
| 1 | nvme | PCI探测（BAR0>4G，待高位MMIO） |
| 1 | bootfs | Limine boot module内存文件系统 |
| 2 | vfs | 挂载bootfs，文件查找/读取 |
| 2 | fat32 | 只读FAT32，优先block provider回退boot module |
| 2 | devfs | /dev/version,platform,fb0,modules,boot/ |
| 3 | ps2kbd | IRQ1，scancode set1→ASCII |
| 3 | e1000 | PCI+MAC+IRQ+完整RX/TX ring+DHCP/ARP/UDP/DNS（polling） |
| 3 | virtio_net | PCI+modern capability枚举（未做virtqueue） |

---

## 7. 文件系统与块设备

- **block provider registry**：`kernel_api.block`注册/读写，AHCI注册`ahci0`。block read按最多8扇区分块。
- **FAT32**：DKM驱动只读（优先block provider，回退boot module）；DSK loader内嵌FAT32（读写：根目录文件创建/覆写/FAT链分配释放，用于USER.CONF/firstInit.txt持久化）
- **devfs**：/dev/version,platform,fb0,modules,boot/

---

## 8. 网络子系统

- netdev registry：8槽固定表，O(1)注册查询
- kernel_api.net：register_netdev/submit_tx/poll_rx
- netman.elf：DSK调度，读conf.conf/NETCONF.CNF，DHCP四步握手+ARP网关+DNS A查询（e1000真实收发，polling）

---

## 9. 图形、字体与首次启动

### 9.1 字体系统

DBF格式8bpp灰度：simhei_16/24/32.dbf（GB2312 6763汉字+ASCII 32-126），索引按codepoint升序二分查找。生成工具`CODE/font/mkfont.py`。FirstInit用`render_embedded.py`预渲染中文位图嵌入ELF。

### 9.2 首次启动流程

```text
DSK → 读FUCK配置(show_logo/skip_login/mouse_init/netman/dev_mode)
  → 渐变背景+旋转加载环(comet-tail弧+TSC计时+show_logo=0可跳过)
  → 读firstInit.txt("0|1\n0|1"，第二行dev_mode)
  → dev_mode=1: 加载shell.elf自动命令测试
  → firstInit==0(首次): mouseInit→netman→FirstInit(偏好/账户/网络)→写USER.CONF+翻转firstInit.txt→desktop
  → firstInit!=0(正常): login.elf(USER.CONF+skip_login可跳)→desktop
```

| 组件 | 产物 |
|------|------|
| FirstInit | `SYSTEM/system/user/use/FirstInit.elf` |
| mouseInit | `SYSTEM/system/deshab64/mouse/mouseInit.elf` |
| netman | `SYSTEM/system/deshab64/network/netman.elf` |
| login | `SYSTEM/system/user/use/login.elf` |
| desktop | `SYSTEM/system/deshab64/desktop.elf` |

---

## 10. 内核启动配置（FUCK）

`SYSTEM/system/deshab64/FUCK`，INI分区格式。

| 阶段 | 读取方式 |
|------|---------|
| UTSM | Limine boot module（cmdline=fuck:config），serial_init+idt_init后 |
| DSK | FAT32子目录遍历（`SYSTEM  /DESHAB64 /FUCK    `），block provider可用后 |

```ini
[boot] vmm=1 linux_guest=1 selftest=1 dev_mode=0
[drivers] timer=1 apic=1 nvme=0   # 0=跳过
[utsm] max_segments=1024 max_pckc_keys=8 dirty_shard_pages=1024 arena_size_mb=16
[dsk] show_logo=1 skip_login=0 default_shell=desktop mouse_init=1 netman=1
```

缺失或空时用硬编码默认值。算法参数通过运行期全局变量覆盖编译期宏，静态数组大小仍用编译期上限。

关键代码：`CODE/UTSM/kernel/ini_parser.h/c`、`CODE/UTSM/kernel/main.c`、`CODE/UTSM/include/utsm/config.h`、`CODE/dsk/main.c`。

---

## 11. 开放设计点

1. **加密原语未指定**——KDF/hash/stream/MAC待定，当前占位XOR stream
2. **kernel_api设计态vs实现态**——RE定义typed sub-struct指针，实现为const void*占位
3. **block未作为独立子API**——走provider注册
4. **log API签名分歧**——定参vs变参，统一方向待定
5. **v1不追求**——RAM原地回滚/强安全隔离/设备副作用回滚

---

## 12. 系统目录约定

```text
SYSTEM/
├── boot/utsm.elf, limine.conf, linux-bzImage, linux-initrd.img, linux-rootfs.img
├── driver/manifest.json, platform/bus/block/*.drv, fs/net/input/console/, test.fat32
├── system/deshab64/deshab.elf, FUCK, desktop.elf, shell.elf, cmd.elf, tools/, mouse/, network/
├── system/font/simhei*.dbf
├── system/user/use/FirstInit.elf, login.elf, firstInit.txt, USER.CONF
└── EFI/BOOT/BOOTX64.EFI

CODE/  UTSM/ dsk/ DKM/ firstInit/ mouse/ netman/ desktop/ shell/ cmd/ tools/ linux/ utsm-ipc/ font/
RE/    UTSM_设计架构.md, UTSM_算法记录.md, 驱动模块ABI设计.md
docs/  （本文档所在）
```

---

## 13. 相关文档索引

| 文档 | 内容 |
|------|------|
| [CODE/UTSM/README.md](../CODE/UTSM/README.md) | UTSM封缄内存模块 |
| [CODE/DKM/README.md](../CODE/DKM/README.md) | DKM驱动模块系统 |
| [CODE/dsk/README](../CODE/dsk/README) | DSK主内核与boot context |
| [RE/UTSM_设计架构.md](../RE/UTSM_设计架构.md) | UTSM架构记录 |
| [RE/UTSM_算法记录.md](../RE/UTSM_算法记录.md) | UTSM核心算法 |
| [RE/驱动模块ABI设计.md](../RE/驱动模块ABI设计.md) | DKM ABI详细设计 |
| [docs/BOOT_SEQUENCE.md](./BOOT_SEQUENCE.md) | 启动路线设计 |
| [docs/ROADMAP.md](./ROADMAP.md) | 开发路线图 |
| [docs/系统开发策划.md](./系统开发策划.md) | 实现矩阵与里程碑策划 |
| [docs/兼容层设计.md](./兼容层设计.md) | Linux/PE兼容层 |
| [docs/桌面设计.md](./桌面设计.md) | 桌面环境设计 |
