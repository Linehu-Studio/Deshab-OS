# Review Context and Use Multi-Agent

> **Workspace:** d:\Code\Deshab

---

## 第一轮 5 智能体推进

### Agent 1: Phase 3 文件传输 IPC

**修改文件**：`CODE/utsm-ipc/ipc_proto.h`, `CODE/UTSM/vmm/hypercall.c`, `CODE/UTSM/vmm/linux_compat.c`, `CODE/linux/patches/utsm_hcall.c`, `CODE/linux/initramfs/bin/utsm_exec_daemon.c`

**双通道协议**：
1. **ring 消息通道**：`FILE_LIST(24)/READ(25)/WRITE(27)REQUEST` + `FILE_RESPONSE(26)`，park-and-resume 同步模型，~1MB `payload_pool` 零拷贝中转
2. **hypercall 直连通道**：`FILE_READ(0x0060)/FILE_WRITE(0x0061)`，Linux 侧直接读写 UTSM FAT32 根目录文件

**分块语义**：单次往返最多搬运 pool 容量（1015712B），大文件递增 `file_offset` 循环；写入约定 `file_offset==0` 即 `O_TRUNC`（整文件替换）

**返回打包**：`FILE_READ` hypercall RAX 低 20 位=暂存字节数、高位=文件总大小

**SHM 布局**：64B header + 2×16400B ring + pool = 恰好 1MB，编译期 `_Static_assert` 守卫

**构建验证**：UTSM 全量重建通过（`utsm.elf` 170384B），Linux daemon 静态编译 827096B，`ipc_proto.h` 三种包含模式全部验证

### Agent 2: virtio-net 网络路径

**修改文件**：`CODE/UTSM/vmm/virtio_net.c`（唯一改动）

**核心 bug 修复**：TX 全坏根因——`net_gather_frame` 原判断 `len > VIRTIO_NET_HDR_LEN` 才跳过头，Linux virtio-net 把 10 字节 hdr 作为独立描述符（len==10）条件不成立 → hdr 拼入待发帧 → 坏包。改为首描述符无条件跳过 `min(len,10)` 字节

**TX 数据路径**：guest QUEUE_NOTIFY(1) → `net_handle_tx` → `net_gather_frame` 跳 hdr、拼接链到 2048B scratch → `net->tx(bound_dev)` 送 e1000 → used 回执 → IRQ6

**RX 数据路径**：1kHz preemption timer → `vmexit_before_resume` → `virtio_net_poll` → `net_handle_rx`：`net_rx_data_region` 解析两种布局（hdr 独立 2 段、hdr+data 同段），校验 W flag；有 buffer 时 `rx_poll` 填入；无包不消耗 buffer；guest 无 buffer 时主动收包丢弃防 e1000 ring 堵死；单次 32 帧 budget

**统计**：rx_filled / rx_drop_nobuf / rx_badbuf / tx_sent / tx_drop，每 512 事件一行

**编译验证**：零 warning 通过

### Agent 3: VMM 核心

**结果**：`vmexit_before_resume` + preemption timer 1ms 周期 exit + interrupt window 注入框架已落盘（汇报丢失但代码在）

### Agent 4: Deaicup 调研

**结论**：推荐路径 B（Linux guest 内 egui 软件渲染 + IPC 拉帧到 Deshab 桌面窗口，类 WSLg）。决策报告已写入 `docs/RE/docs/deaicup_runtime_decision.md`

**依据**：Deaicup 是 eframe 0.27.2 应用，wgpu/glow 双 GPU 后端在无 GPU 环境均不可用；HTTPS 图片加载恰是 Linux guest 已有能力。需新增显示 SHM（800×600 RGBA 1.83MB，建议新 GPA 8MiB）+ 输入转发 + 桌面远程窗口

### Agent 5: FUCK 自定义内核配置

**修改文件**：`CODE/UTSM/kernel/main.c`, `CODE/UTSM/dkm/manifest.c`, `CODE/dsk/main.c`, `CODE/UTSM/arch/x86_64/idt.c`

**框架构成**：
- `ini_parser.c/h`：freestanding 无堆分配，64 条目上限，接口 `ini_parse/ini_get/ini_get_int/ini_get_bool`
- FAT32 子目录遍历：`fat32_find_in_dir` + `fat32_read_path`，支持 `/` 分隔 8.3 路径
- UTSM 经 Limine boot module 读取（cmdline="fuck:config"）；DSK 经 FAT32 自读
- DSK 侧根目录回退：先 `/SYSTEM/DESHAB64/FUCK`，失败再试根目录 `FUCK`

**已接入插桩点**：
1. `[utsm]` 算法参数 → 5 个运行期全局变量覆盖编译期默认值（`max_segments=0x400` 已验证）
2. `[drivers]` 驱动过滤 → `dsm_load_by_manifest_ex` 按名跳过（nvme/virtio_net/ath9k 被 `disabled by FUCK`）
3. `[boot]` selftest/vmm/linux_guest → 三个启动开关
4. DSK 侧 `[dsk]` show_logo/skip_login/mouse_init/netman + `[boot]` dev_mode

## 第二轮 3 智能体推进

### Agent A: net.c 日志刷屏 + mkfat32.ps1 加入 FUCK

1. `CODE/UTSM/kernel/net.c`：删除 `net_tx_impl` 中每包 `log_hex64("[NET] tx dispatch fn=")` 
2. `.build_tmp/mkfat32.ps1`：`$fileDefs` 末尾追加 `@{ Name83 = 'FUCK       '; Path = ...FUCK }`

### Agent B: NVMe 高位 BAR 映射 + 4G+ BAR 失败根因

**现状核实**：`paging.c`/`mmio.c`/`mm.h`/`paging.h` 均已完整可用

**4G+ BAR 失败根因**：
1. Limine HHDM 保证覆盖 RAM 但不保证覆盖高位 PCI MMIO 洞
2. HHDM 映射是 WB cacheable，MMIO 需要 PCD|PWT（UC）属性

**mm_map_mmio 设计**（`CODE/UTSM/mm/mmio.c` + `CODE/UTSM/include/utsm/mm.h`）：
- 4 级页表（LA57 拒绝），运行时扫描 PML4[256..510] 找空闲槽（512GiB）
- 叶子 PTE = `P|RW|PCD|PWT`（UC）；map/unmap 均 `invlpg`
- 冲突：walk 撞大页→失败；PTE 已 present→回滚；unmap 只清自己建的 4K PTE
- 懒初始化（首次 map 时读 CR3/HHDM）

**kernel_api 追加**：`mmio`@+0xB0（并行 agent）、`mm_map_mmio`@+0xB8、`mm_unmap_mmio`@+0xC0

**dkm_shared.h 修复**：末尾追加 `mmio`/`mm_map_mmio`/`mm_unmap_mmio` 三个字段

### Agent C: NVMe 驱动 bug 修复

**三个 bug**：
1. Create IOCQ/IOSQ 的 **CDW10 高低半字写反**：Spec 规定 `CDW10 = QSIZE[31:16] | QID[15:0]`，原代码 `(1u << 16) | (IO_QSIZE - 1u)` 把 QID 放到高半字
2. **错误码丢失 SCT**：`return (int)sc` 在 SC=0/SCT≠0 时返回 0 → 改为 `return (int)(sc | (sct << 8))`
3. **Read opcode 误为 Flush**：`NVME_IO_READ=0x00` 应为 `0x02`（NVM 命令集：00h=Flush, 01h=Write, 02h=Read）

**修复后验证**：`map_mmio` 高位 BAR 映射成功 → admin/IO queue live → READ LBA0 rc=0，DESHABNVME0 签名验证通过 → nvme0 block provider 注册

### Agent D: desktop 集成 Linux 命令为应用图标

**修改文件**：`CODE/tools/desktop_app.h`, `CODE/desktop/main.c`

- `app_descriptor` 增加 `linux_cmd` 字段
- `MAX_APPS` 8→16、`MAX_ICONS` 8→16
- `LINUXAPP.CNF`（8.3名 LINUXAPPCNF）：格式 `显示名|linux命令`
- 缺失时回退默认 NEOFETCH+HTOP
- `lnx_on_create`：同步 `exec()` 阻塞 → 输出经 ANSI CSI 剥离/`^M` 忽略/`\t` 展开后一次性追加
- `draw_icon_linux`：暗底 + 霓虹绿 `>_`

### Agent E: PS/2 鼠标完整实现

**修改文件**：`CODE/mouse/main.c`（从安全 stub 重写为完整实现，318 行）

**初始化序列**：ps2_drain → 读配置字节 → 启用 AUX → 复位 → Set Defaults → 采样率 100 → 分辨率 2 → 启用数据报告

**IRQ12 包解码**：经 `api->irq_register(12, handler)`；读 0x60 前查 AUX 位；3 字节包解码（同步位验证、溢出检测、9 位有符号位移合成）；坐标钳位到 framebuffer 边界

**`mouse_state_t`**（导出 BSS 符号，ABI v1）：magic/version/x/y/buttons/packet_count/overflow_count/max_x/max_y。另提供 `mouse_poll_state()` 只读快照

**handler 存活性**：只注册、不解屏蔽 PIC、不 sti。DSK 用同一块 1MB `ibuf` 加载所有模块，mouseInit 返回后镜像被覆盖

## 整合构建 + QEMU 验证

**desktop.elf 加载失败修复**：BSS 段膨胀（8 个 Linux 窗口槽 + 16KB 缓冲 + MAX_APPS 翻倍）导致 memsz span 1.66MB 超过 `ibuf[1048576]`（1MB）。修复：`ibuf` 扩大到 2MB

**端到端验证结果**：UTSM+FUCK → 14 驱动（NVMe 高位 BAR 映射成功）→ SELFTEST PASS → VMM 优雅降级 → DSK → FUCK 根目录回退命中 → mouseInit → netman（DHCP 10.0.2.15 + ARP + DNS 解析成功）→ FirstInit 账户卡片

**已知遗留**：DSK 加载偶发 `ELF rejected`（AHCI 单扇区读取偶发超时，重跑即可恢复）

## host DKM/框架层发现的问题（未改，仅报告）

1. `e1000.c` `e1000_tx` 每包 3 行 `raw_log` 串口打印，TX 吞吐被压到 KB/s 级
2. `vmexit.c` `maybe_inject_irq` 在 IMR mask 时直接丢弃 pending IRQ，guest mask IRQ6 可能丢一次中断
3. MAC 共用架构限制：guest virtio-net 回填 host e1000 的 MAC，QEMU slirp 下 host/guest 同 MAC 同租约
