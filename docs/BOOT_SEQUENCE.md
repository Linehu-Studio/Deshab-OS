# Deshab 启动路线设计（B0–B4 演示版）

> 从上电到用户可交互的完整启动序列，带契约的分阶段唤醒过程。
> 说明：本文为 QEMU 演示版——"契约/锚点/回退链"描述的是 QEMU 下的实际行为，非生产级承诺。状态见 [STATUS.md](./STATUS.md)。

---

## 0. 设计理念

启动序列命名为**DBS（Deshab Boot Sequence）**，五阶段：

| 阶段 | 代号 | 含义 | 产物 |
|------|------|------|------|
| B0 | Ignis 火种 | 固件→引导器 | utsm.elf入内存 |
| B1 | Sigillum 封缄解封 | UTSM建立封缄内存+驱动底座 | 14 DKM驱动ACTIVE |
| B2 | Transitus 交接 | UTSM→DSK版本化传递 | dsk_boot_context就绪 |
| B3 | Origo 溯源 | DSK判定首次/正常启动 | firstInit标志判定 |
| B4 | Excitas 唤醒 | 用户设置向导或正常启动 | 用户可交互 |

三个核心概念：
- **启动契约**：每阶段Entry/Exit/Fail承诺
- **启动锚点**：阶段结束的可验证串口日志标记（稳定点）
- **启动回退链**：失败降级路径（B2 FAT32失败→Limine module；B1 required驱动失败→DRR/panic；optional→标记FAILED继续）

---

## 1. 启动总览

```text
B0 Ignis: UEFI/BIOS → Limine(读limine.conf, 加载utsm.elf, 预加载17个boot module)
B1 Sigillum: serial/IDT/arena/DMA → UTSM init+PCKC → DKM 4-stage 14驱动 → selftest锚点
B2 Transitus: FAT32 block provider读deshab.elf(失败→Limine module) → ELF校验+PT_LOAD → 填充dsk_boot_context → 跳转DSK
B3 Origo: 渐变背景+静态Logo → FAT32读firstInit.txt → 首次/正常分支
B4 Excitas(首次): mouseInit → netman → FirstInit → 写USER.CONF+firstInit.txt → desktop
B4 Excitas(正常): login → desktop
```

---

## 2. B0 Ignis — 火种

**契约**：Entry=CPU上电/UEFI；Exit=utsm.elf加载+boot module table构建；Fail=Limine找不到utsm.elf→引导失败

Limine加载utsm.elf为内核，预加载17个boot module（1 manifest+14 .drv+1 deshab.elf+1 test.fat32）。

未来扩展：BCB（Boot Control Block）CRC校验→active_slot选择→失败次数超限切last_good_slot。

---

## 3. B1 Sigillum — 封缄解封

**契约**：Entry=Long mode+_start获控/中断禁用；Exit=UTSM初始化+14驱动ACTIVE+selftest通过；Fail=required→DRR/panic，optional→FAILED继续

**入口约定**（boot.S）：`cli; cld`必须最前（未建IDT前中断→三重故障）；早期设IA32_GS_BASE（防栈保护prologue页错误）；未启用SSE前-mno-sse -msoft-float。

```text
kernel_main() → serial_init → idt_init → arena_init(16MB) → dma_init
  → net_init(8槽) → drr_stub_init → utsm_init → dkm_init+fill_platform_info
  → dsm_load_by_manifest(4 stage) → utsm_selftest_run → dsk_load_and_jump
```

**DKM分阶段加载**：stage0 platform(required→panic) / stage1 boot(required→DRR) / stage2 filesystem(fallback bootfs) / stage3 optional(FAILED继续)

加载状态机：DISCOVERED→QUEUED→DEP_WAIT→LOADING→ELF_CHECKED→MEMORY_ALLOCATED→RELOCATED→ABI_CHECKED→INITING→ACTIVE

ELF重定位：必须64/RELATIVE/GLOB_DAT/JUMP_SLOT，禁止TLS/IFUNC/lazy binding。

B1完成后kernel_api暴露：log/rsdp/fb_*/boot_modules/irq_register/hhdm/dma.alloc_pages/block/net

**B1锚点**：`[UTSM] SELFTEST PASS`

---

## 4. B2 Transitus — 交接

**契约**：Entry=B1锚点达成；Exit=deshab.elf加载重定位+dsk_boot_context填充→dsk_entry(ctx)；Fail=FAT32失败→Limine module，两者皆失败→panic

```text
dsk_load_and_jump():
  ① 优先FAT32 block provider: kernel_api.block.read→解析BPB→遍历root→找"DESHAB  ELF"→沿FAT链读取
  ② 回退Limine boot module: 找path=="/system/deshab64/deshab.elf"或cmdline=="dsk:main"
  ③ ELF校验: ELFCLASS64/ELFDATA2LSB/EM_X86_64/ET_DYN|ET_EXEC
  ④ PT_LOAD: min/max vaddr→arena分配→清零→复制filesz
  ⑤ 填充dsk_boot_context → ⑥ 跳转entry(ctx)
```

关键约束：`data_start_sec=rsvd+fat_count*spf`（漏第二份FAT→root cluster错位）；arena需足够（16MB）；批量读256扇区稳定，单扇区偶发超时。

**dsk_boot_context**（magic=`0x44534B31424F4F54` "DSK1BOOT"，abi_version=1）：

```c
struct dsk_boot_context {
    u64 magic, flags;  u32 abi_version, size;
    u64 hhdm_offset, rsdp_address;
    u64 framebuffer_address/width/height/pitch/bpp;
    u64 boot_modules_response, dkm_kernel_api;
    u64 dkm_driver_table/driver_count;
    u64 utsm_state/drr_state, memory_map/count/entry_size;
    u64 kernel_stack_top;
    u64 reserved[8];  // 只能追加不能插入
};
```

跳转约定：RDI=ctx*, RSI=RDX=0, IF=0, paging=UTSM页表

**ABI约束**：reserved区只能追加不能插入；block_read通过硬偏移api+0xA8读取。

**B2锚点**：`[DSK] boot` + `[DSK] context ok`

---

## 5. B3 Origo — 溯源

**契约**：Entry=B2锚点+framebuffer可用；Exit=firstInit标志判定；Fail=无block device→halt；firstInit.txt不存在→视为首次

```text
dsk_entry(ctx):
  ① cli（IRQ期间安全）
  ② 校验ctx->magic
  ③ 取block_read(api+0xA8)
  ④ 读FUCK配置(FAT32子目录)
  ⑤ 渐变背景(BG_TOP淡蓝→BG_BOTTOM深紫)
  ⑥ 静态Logo(show_logo=1时): Logo.png居中alpha混合绘制于渐变背景（旋转加载环已移除）
  ⑦ 首次启动检测: FAT32读firstInit.txt("0|1\n0|1")，firstInit=='0'→首次
  ⑧ dev_mode=1→加载shell.elf自动命令测试
  ⑨ skip_firstinit=1→直达desktop；否则按首次/正常分支调度
```

Logo绘制：居中 + alpha混合（B3 静态画面，取代原 comet-tail 旋转环）。

**B3锚点**：`[DSK] FUCK config loaded` + `[DSK] firstInit=0`(或"!=0"/"not found")

---

## 6. 系统初始化器调度协议

DSK独占系统组件初始化职责，FirstInit只负责用户级设置。

首次启动调度：①mouseInit.elf(FUCK mouse_init=0可跳)→②netman.elf(FUCK netman=0可跳)→③FirstInit.elf→④dsk_persist_userconf→⑤desktop.elf

**ELF加载**：DSK内嵌FAT32+PIE ELF loader→校验ELF64/EM_X86_64→PT_LOAD→PT_DYNAMIC→R_X86_64_RELATIVE重定位→entry(ctx)

关键约束：PIE ELF必须处理R_X86_64_RELATIVE；ELF≤256KB(g_fdata[262144])；8.3文件名用栈上char[]构造避免PIE重定位。

| 初始化器 | 状态 | 职责 |
|---------|------|------|
| mouseInit | 安全stub | 只读PS/2状态，不写控制器 |
| netman | 协议栈闭环 | 配置加载+netdev枚举+DHCP/ARP/DNS |
| FirstInit | UI+输入 | 偏好/账户/网络设置→SHA256密码→返回DSK |

---

## 7. B4 Excitas — 唤醒

### 7.1 首次启动

```text
FirstInit: 接收ctx→环淡出(~1s)→"欢迎使用Deshab"→等待5s→偏好设置页
  →账户设置卡片(圆角矩形/白色输入框/18px中文)→键盘输入(计算机名/用户名/密码)
  →网络配置页(WiFi)→SHA256摘要+XOR加密→返回DSK
DSK: 写USER.CONF+翻转firstInit.txt('0'→'1')→desktop.elf
```

字体：预渲染中文位图(render_embedded.py)嵌入ELF；运行时DBF格式(simhei_16/24/32.dbf)。

### 7.2 正常启动

```text
firstInit!=0 → dev_mode=1先shell.elf → login.elf(USER.CONF+skip_login可跳) → desktop.elf
```

desktop.elf：多页面+窗口管理+任务栏+双命令行+工具应用（见[桌面设计](./桌面设计.md)）。

### 7.3 首次→正常转换

`dsk_persist_userconf`：经ctx->reserved[0/1]传入的加密配置缓冲写FAT32根目录USER.CONF；读firstInit.txt保留第二行dev_mode，首字节'0'→'1'覆写。依赖kernel_api.block write(AHCI WRITE DMA EXT)+fat32_write_root_file。

**B4锚点**：`[DSK] loading desktop`

---

## 8. 启动错误恢复

| 失败点 | 回退策略 |
|--------|---------|
| B2 FAT32不可用 | 回退Limine module |
| B2 DSK两路径都找不到 | panic |
| B3 无block device | halt |
| B3 firstInit.txt不存在 | 视为首次启动 |
| B4 mouseInit/netman加载失败 | 跳过继续 |
| B4 FirstInit加载失败 | 直接desktop |
| B4 login缺失/失败 | 跳过login |
| B1 required驱动失败 | DRR/panic |
| B1 optional驱动失败 | FAILED继续 |

DRR接管后升级为三级：Page Rollback→Segment Rollback→System Rollback。

---

## 9. 启动状态传递链

```text
Limine response → UTSM kernel_main → dkm_kernel_api(log/irq/dma/block/net/...)
  → dsk_boot_context → DSK(同一ctx传给所有初始化器)
  → block_read(api+0xA8) → ahci0 provider
```

关键约束：DSK及初始化器共享同一dsk_boot_context；block_read硬偏移0xA8（错误→NULL解引用）；所有初始化器为PIE ELF须处理R_X86_64_RELATIVE。

---

## 10. 启动阶段与源码映射

| 阶段 | 源码 |
|------|------|
| B0 | `SYSTEM/EFI/BOOT/` `SYSTEM/boot/limine.conf` |
| B1 | `CODE/UTSM/` `CODE/DKM/` |
| B2 | `CODE/UTSM/kernel/dsk_loader.c` `block.c` |
| B3 | `CODE/dsk/main.c` |
| B4 | `CODE/firstInit/` `CODE/mouse/` `CODE/netman/` `CODE/font/` |

---

## 11. 启动验证标准

**最小验证**（QEMU默认）：
```text
[UTSM] boot → [UTSM] SELFTEST PASS → [UTSM] jumping to DSK
→ [DSK] boot → [DSK] context ok → [DSK] loading desktop
```

**首次启动完整验证**（带SATA FAT32）：
```text
...→ [DSK] firstInit=0 → [DSK] first init detected
→ mouseInit → netman → FirstInit → USER.CONF write → firstInit.txt flip
→ [DSK] loading desktop
```

设备测试场景：SATA块设备(block_read非NULL)、NVMe(1b36:0010)、e1000(tx=ready rx=ready)、virtio-net(1af4:1001)。

---

## 12. 相关文档

| 文档 | 内容 |
|------|------|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | 五大核心子系统 |
| [ROADMAP.md](./ROADMAP.md) | 开发路线图 |
| [桌面设计.md](./桌面设计.md) | 桌面环境 |
| [兼容层设计.md](./兼容层设计.md) | Linux/PE兼容层 |
| [CODE/UTSM/README.md](../CODE/UTSM/README.md) | UTSM封缄内存 |
| [CODE/DKM/README.md](../CODE/DKM/README.md) | DKM驱动系统 |
