# Review Contexts and Multi-Agent Progression

> **Workspace:** d:\Code\Deshab

---

## 第一轮 5 智能体推进

### Agent A: virtio-net 数据路径重写

**核心 bug**：TX 全坏根因——`net_gather_frame` 原判断 `len > VIRTIO_NET_HDR_LEN` 才跳过头，Linux virtio-net 把 10 字节 hdr 作为独立描述符（len==10）条件不成立 → hdr 拼入待发帧 → 坏包

**修改**：`CODE/UTSM/vmm/virtio_net.c` 唯一改动，重写 RX/TX 数据路径核心函数 + 统计

**TX**：guest QUEUE_NOTIFY(1) → `net_handle_tx` → `net_gather_frame` 跳 hdr、拼接链到 2048B scratch（超容量整帧丢弃）→ `net->tx(bound_dev)` → bump used → IRQ6

**RX**：1kHz preemption timer → `virtio_net_poll` → `net_handle_rx`：`net_rx_data_region` 解析两种布局（hdr 独立 2 段、hdr+data 同段），校验 W flag；有 buffer 时 `rx_poll` 填入；无包不消耗 buffer；guest 无 buffer 时主动收包丢弃防 e1000 ring 堵死；异常 buffer len=0 回执；单次 32 帧 budget

**统计**：rx_filled / rx_drop_nobuf / rx_badbuf / tx_sent / tx_drop，每 512 事件串口一行

**host DKM 发现的问题**（未改）：
1. `e1000.c` `e1000_tx` 每包 3 行 `raw_log` 串口打印 → TX 吞吐被压到 KB/s 级
2. `vmexit.c` `maybe_inject_irq` 在 IMR mask 时直接丢弃 pending IRQ
3. MAC 共用架构限制：guest virtio-net 回填 host e1000 的 MAC

### Agent B: 4G+ BAR 失败根因 + mm_map_mmio

**根因**：
1. Limine HHDM 保证覆盖 RAM 但不保证覆盖高位 PCI MMIO 洞（QEMU/OVMF 把 64-bit BAR 放到 ~48GiB 附近）
2. HHDM 映射是 WB cacheable，MMIO 需要 PCD|PWT（UC）属性

**mm_map_mmio**（`CODE/UTSM/mm/mmio.c` + `CODE/UTSM/include/utsm/mm.h`）：
- 4 级页表（LA57 拒绝），运行时扫描 PML4[256..510] 找空闲槽（512GiB）
- 叶子 PTE = `P|RW|PCD|PWT`（UC）；map/unmap 均 `invlpg`
- 冲突：walk 撞大页→失败；PTE 已 present→回滚
- 懒初始化（首次 map 时读 CR3/HHDM）

**kernel_api 追加**：`mmio`@+0xB0（并行 agent）、`mm_map_mmio`@+0xB8、`mm_unmap_mmio`@+0xC0

**dkm_shared.h 修复**：末尾追加 `mmio`/`mm_map_mmio`/`mm_unmap_mmio` 三个字段

**并行 agent 撞车处理**：另一 agent 创建了 `paging.c/h`（`dkm_mmio_api`：is_mapped/map_mmio/cr3，HHDM 空洞原地补 4K UC 页）并重写了 nvme.c。两套接口并存不冲突：paging.c 在 HHDM 槽内补页；mmio.c 用独立空闲槽，独有 unmap 和"HHDM 已被 WB 大页覆盖时的 UC 属性修正"能力

### Agent C: desktop 集成 Linux 命令为应用图标

**修改文件**：`CODE/tools/desktop_app.h`, `CODE/desktop/main.c`

- `app_descriptor` 增加 `linux_cmd` 字段（互斥：elf_name 优先，仅设 linux_cmd 时由内置终端窗口承载）
- `MAX_APPS` 8→16、`MAX_ICONS` 8→16
- `LINUXAPP.CNF`（8.3名 LINUXAPPCNF）：格式 `显示名|linux命令`
- 缺失时回退默认 NEOFETCH+HTOP
- `lnx_on_create`：bash_reset → 标题 → `$ <cmd>` 回显 → 同步 `exec()` 阻塞 → 输出经 ANSI CSI 剥离/`^M` 忽略/`\t` 展开后一次性追加 → `[exit N]`
- `draw_icon_linux`：暗底 + 霓虹绿 `>_`

**接口偏差**：任务描述的回调式 `exec(..., out_cb, err_cb, ...)` 不存在，真实接口是缓冲式 `exec(path, argc, argv, stdout_buf, cap, &len, &exit_code)`

**遗留风险**：exec 期间 UI 完全冻结；stdout 16KB 截断；非 ASCII 字节钳为空格；htop/top 等交互式 TUI 会阻塞到超时

### Agent D: NVMe 驱动 bug 修复

**三个 bug**：
1. Create IOCQ/IOSQ 的 **CDW10 高低半字写反**：Spec 规定 `CDW10 = QSIZE[31:16] | QID[15:0]`，原代码把 QID 放到高半字 → QEMU 实际创建 CQ63（2 项）→ Create IOSQ 引用 CQID=1 不存在 → `CQID_INVALID`
2. **错误码丢失 SCT**：`return (int)sc` 在 SC=0/SCT≠0 时返回 0 → 改为 `return (int)(sc | (sct << 8))`
3. **Read opcode 误为 Flush**：`NVME_IO_READ=0x00` 应为 `0x02`

**修复后**：`map_mmio` 高位 BAR 映射成功 → admin/IO queue live → READ LBA0 rc=0，DESHABNVME0 签名验证通过 → nvme0 block provider 注册

### Agent E: PS/2 鼠标完整实现

**修改文件**：`CODE/mouse/main.c`（从安全 stub 重写为完整实现，318 行）

**初始化序列**：ps2_drain → 读配置字节 → 启用 AUX → 复位 → Set Defaults → 采样率 100 → 分辨率 2 → 启用数据报告

**IRQ12 包解码**：经 `api->irq_register(12, handler)`；读 0x60 前查 AUX 位（键盘数据留给键盘路径）；3 字节包解码（同步位验证、溢出检测、9 位有符号位移合成）；坐标钳位到 framebuffer 边界

**`mouse_state_t`**（导出 BSS 符号，ABI v1）：magic/version/x/y/buttons/packet_count/overflow_count/max_x/max_y。另提供 `mouse_poll_state()` 只读快照

**handler 存活性**：只注册、不解屏蔽 PIC、不 sti。DSK 用同一块 1MB `ibuf` 加载所有模块，mouseInit 返回后镜像被覆盖

**兼容性**：FirstInit/desktop 均自带幂等重初始化 + AUX 位感知轮询，netman 不碰 0x60/0x64

## 第二轮 3 智能体推进

### Agent A: net.c 日志刷屏 + mkfat32.ps1 加入 FUCK

1. `CODE/UTSM/kernel/net.c`：删除 `net_tx_impl` 中每包 `log_hex64("[NET] tx dispatch fn=")`
2. `.build_tmp/mkfat32.ps1`：`$fileDefs` 末尾追加 `@{ Name83 = 'FUCK       '; Path = ...FUCK }`

### Agent B: Deaicup 运行环境决策

**结论**：推荐路径 B（Linux guest 内 egui 软件渲染 + IPC 拉帧到 Deshab 桌面窗口，类 WSLg）

**依据**：Deaicup 是 eframe 0.27.2 应用，wgpu/glow 双 GPU 后端在无 GPU 环境均不可用；HTTPS 图片加载恰是 Linux guest 已有能力。需新增显示 SHM（800×600 RGBA 1.83MB，建议新 GPA 8MiB）+ 输入转发 + 桌面远程窗口

### Agent C: NVMe 全链路验证

**已核实文件完整可用**：
- `paging.c`：`is_mapped` 四级页表只读 walk + `map_mmio` HHDM 空洞补 4K UC 页 + `cr3`
- `mmio.c`：高半部 PML4[256..510] 扫空闲槽做独立 MMIO 窗口 + bump 分配 + 冲突回滚
- `kernel_api.c`：`.mmio = paging_get_api()`、`.mm_map_mmio`、`.mm_unmap_mmio` 均在 `dkm_fill_platform_info` 填充
- `nvme.c`：完整实现到 block provider（探测→高位 BAR 映射→控制器初始化→admin/IO queue→READ LBA0→nvme0 block provider 注册）

**NVMe 驱动状态**：
- 探测：按 class code `01/08/02` 全总线扫描 → 64-bit BAR0 解析 → PCI MEM+BUSM 使能
- 高位 BAR：`is_mapped` 四级 walk 零 #PF 风险核实 → 未覆盖则 `map_mmio(bar_phys, 8K)` 补 UC 页
- 控制器初始化：CAP/VS 校验 → EN=0 等 RDY=0 → AQA/ASQ/ACQ → CC=0x00460001 → RDY=1
- Admin queue：Identify Controller + Identify Namespace（校验 9≤lbads≤12）
- IO queue：Create IO CQ/SQ（qid=1、64 项、轮询无中断）
- READ：单 PRP1 + 4K bounce、CQE phase bit 轮询 + TSC 超时

**仍缺**：写路径、PRP list（>4K 传输）、MSI/MSI-X、多 NS/多控制器

## 整合构建 + QEMU 验证

**desktop.elf 加载失败修复**：BSS 段膨胀（8 个 Linux 窗口槽 + 16KB 缓冲 + MAX_APPS 翻倍）导致 memsz span 1.66MB 超过 `ibuf[1048576]`。修复：`ibuf` 扩大到 2MB

**QEMU 验证**：UTSM+FUCK → 14 驱动（NVMe 高位 BAR 映射成功）→ SELFTEST PASS → VMM 优雅降级 → DSK → FUCK 根目录回退 → mouseInit → netman（DHCP 10.0.2.15 + ARP + DNS 解析成功）→ FirstInit

**已知遗留**：DSK 加载偶发 `ELF rejected`（AHCI 单扇区读取偶发超时）
