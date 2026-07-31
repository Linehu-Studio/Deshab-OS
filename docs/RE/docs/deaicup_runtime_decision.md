# Deaicup Software 运行环境决策报告

## 1. 概述

Deaicup Software（`CODE/Deaicup Software/`）是一个基于 Rust egui/eframe 框架的 GUI 应用，
目标是在 Deshab 系统上运行。本文基于对 Deaicup 全部源码（`Cargo.toml`、`Cargo.lock`、
`src/main.rs`）的实际阅读结果，评估三条运行路径：

- **路径 A**：Rust/egui 原生移植到 SAS-R0 freestanding 环境
- **路径 B**：在 Linux 兼容层 guest 内运行，Deshab 桌面通过 IPC 共享内存拉帧显示（类 WSLg）
- **路径 C**：用 Deshab 原生 deshab_ui.h 以 C 语言重写

**结论速览**：推荐**路径 B**。Deaicup 当前唯一的非平凡运行时依赖是 HTTPS 图片加载，
而这恰好是 Linux guest 已具备（virtio-net 数据路径闭环）而 SAS-R0 原生完全不具备的能力；
路径 B 全部构件（IPC ring、payload_pool、park-and-resume、文件传输）均已在 Phase 3 验证，
只需新增"显示共享内存 + 输入转发"两个模块。路径 A 需要为 egui 0.27 移植 Rust std 子集，
工作量与风险均为最高；路径 C 当前改写量小但意味着放弃上游 Rust 代码库并长期自行演进 GUI 组件库。

## 2. Deaicup Software 源码事实

### 2.1 项目结构与依赖

项目根目录为 `CODE/Deaicup Software/src/`（注意多一层 `src`），全部源码仅一个文件
`src/src/main.rs`（102 行）。`Cargo.toml` 关键事实：

```toml
[package]
name = "Deaicup-Software"
version = "0.1.0"
edition = "2024"

[[bin]]
windows-subsystem = "windows"   # Windows GUI 子系统，无控制台窗口

[dependencies]
egui = "0.27"
eframe = "0.27"
egui_extras = { version = "0.27", features = ["all_loaders"] }
```

`Cargo.lock` 锁定的实际版本与传递依赖（与本决策相关的部分）：

| 组件 | 锁定版本 | 说明 |
|---|---|---|
| egui / epaint / emath / ecolor | 0.27.2 | 即时模式 GUI 核心，epaint 负责形状 tessellation |
| eframe | 0.27.2 | 应用框架（窗口/事件循环/后端胶水） |
| egui-wgpu + wgpu | 0.27.2 / 0.19.4 | eframe 默认 feature 之一：wgpu 后端 |
| egui_glow + glow + glutin | 0.27.2 / 0.13.1 / 0.31.3 | eframe 默认 feature 之二：OpenGL 后端 |
| winit | 0.29.15 | 窗口与输入抽象（经 egui-winit） |
| egui_extras | 0.27.2 | `all_loaders` 引入图片加载器链 |
| ehttp | 0.5.0 | HTTP 图片加载；native 下走 ureq |
| ureq + rustls + webpki-roots | 2.12.1 / 0.23.42 / 0.26.11+1.0.9 | HTTPS 协议栈（纯 Rust TLS） |
| image | 0.24.9 | PNG/JPEG 等解码 |
| resvg / usvg | （lock 内） | SVG 加载器 |
| arboard / smithay-clipboard | 3.6.1 | 剪贴板（egui-winit 依赖） |
| accesskit | 0.12.3 | 无障碍（egui 默认依赖） |

关键推论：**eframe 使用的是默认 features，wgpu 与 glow 两个 GPU 后端都在依赖树中**，
且运行时会优先尝试 wgpu（需要 Vulkan/GL/DX），在无 GPU 环境下回退 glow + glutin
（仍需要 OpenGL 实现）。**两个后端都不可能在无 GPU、无 Mesa 的环境中工作**，
因此无论走哪条路径，最终都必须绕开 eframe 的 GPU 后端，直接用 egui + epaint
自写软件渲染集成（epaint 输出的是与后端无关的三角形网格 + 字体图集，可纯 CPU
光栅化到像素缓冲）。

### 2.2 功能点清单（main.rs 逐点核对）

1. **窗口**：`eframe::run_native`，标题 "Deaicup Software"，初始内尺寸 800×600，
   单 viewport，无子窗口、无边框定制。
2. **启动回调**：`egui_extras::install_image_loaders(&cc.egui_ctx)` —— 注册
   HTTP/文件/SVG 图片加载器。
3. **顶部横幅**：`TopBottomPanel::top`，`ui.image()` 直接加载
   **`https://deaicup.com/DeaicupSoftware/top.png`**，拉伸到面板宽 × 75px。
   这是全程序**唯一的网络访问点**，也是 egui_extras `all_loaders` 存在的理由。
4. **左侧导航**：`SidePanel::left`，默认宽 200、可拖拽；7 个 `selectable_label`。
5. **中央页面**：`match current_page`，7 个页面（Home/Tool/Game/LinuxWare/
   WindowsWare/Set/About）**目前全部是占位 stub**（仅一个 `ui.label`）。
6. **状态**：仅 `current_page: Page` 一个字段（`#[derive(Default)]`）。
   **无文件系统访问、无持久化、无多线程、无系统调用级需求**。

### 2.3 运行需求推导

| 需求 | 事实 | 备注 |
|---|---|---|
| 渲染 | epaint 三角形网格 + RGBA 字体图集 | 软件光栅化即可，无需 GPU |
| 浮点 | egui/epaint 大量使用 f32 | x86_64 Rust 基线要求 SSE2 |
| 堆分配 | 大量小对象（Vec/HashMap/String） | 需要 alloc 级分配器 |
| 时间 | `std::time::Instant`（帧间隔/动画） | TSC 即可提供 |
| 网络 | 仅一处 HTTPS GET（top.png）+ PNG 解码 | ureq + rustls + image crate |
| 文件系统 | **无** | 图片加载器支持 file:// 但代码未用 |
| 窗口 | 单个 800×600 viewport | 无拖拽系统窗口需求 |
| 输入 | 鼠标（移动/点击/滚轮）+ 键盘 | egui RawInput 事件 |
| 剪贴板 | egui-winit 默认接入 | 可降级为 no-op |

## 3. 三条路径评估

### 3.1 路径 A：Rust/egui 原生移植到 SAS-R0

#### 需要做的事

1. **Rust 工具链与运行时**：新建 `x86_64-unknown-none`（或自定义 target JSON）
   交叉编译配置。前置条件：内核启用 FPU/SSE（当前早期启动约束为
   `-mno-sse -mno-sse2 -mno-mmx -msoft-float`，x86_64 Rust 基线硬性要求 SSE2）、
   提供 `alloc_error_handler`、`panic_handler`、全局分配器。
2. **std 移植（最大障碍）**：egui 0.27 **不是 no_std crate**。其依赖树中
   emath/ecolor 可 no_std，但 egui/epaint 本体使用 `std::collections`
   （经 ahash）、`std::time::Instant`、线程相关原语等。可行的做法是为
   Deshab 写一个 Rust std 的 OS 层（类似各 hobby OS 的 std port），把线程/
   Mutex 降级为单线程 stub、IO 降级为无操作、时间接 TSC。这是一块独立的、
   以月计的基础设施工程，且与 egui 版本升级长期耦合。
3. **软件光栅器**：egui 0.27 不内置 CPU 光栅器，需自写 epaint 网格 → RGBA
   光栅化（含字体图集采样、抗锯齿三角填充），或移植 tiny-skia。
   约 500–1500 行，技术风险低但属于新增维护面。
4. **集成层**：PS/2 鼠标/键盘 → `egui::RawInput`；framebuffer 呈现；与
   desktop.elf 窗口系统对接（需要新的 .elf 加载约定：Rust rlib 静态链接成
   PIE ELF，或走 dt_load_elf 兼容的 PIE 布局）。
5. **网络图片**：`https://deaicup.com/...` 需要 TCP + TLS + PNG。SAS-R0 原生
   网络栈尚不存在（e1000/virtio-net 仅到驱动探测/数据路径规划阶段），
   rustls 移植又依赖路径 A 的 std 移植。**短期只能砍掉横幅或改为内嵌静态图**。

#### 评估

- **工作量**：极大（Rust std port 2–4 人月起步，光栅器 + 集成另计）。
- **技术风险**：高。std port 与 egui 版本强耦合；SAS-R0 单地址空间 Ring0 下
  Rust 的安全假设（如线程模型）全部需要重新审视；任一环节卡住则整体不可用。
- **收益**：长期战略收益大（为 Deshab 打开 Rust 生态），但对"运行 Deaicup"
  这一目标而言投入产出比最低。
- **对 SAS-R0 侵入性**：高（SSE 全局启用、新 loader 约定、分配器进驻内核地址空间）。

### 3.2 路径 B：Linux guest 内运行 + IPC 拉帧（类 WSLg）

#### 架构

```
┌──────────────── Deshab (UTSM + desktop.elf) ────────────────┐
│  桌面窗口 "Deaicup"  ──blit──▶ 显示 SHM（guest 渲染的 RGBA）  │
│  PS/2 键鼠 ──ring 消息──▶ linux_to_utsm / utsm_to_linux      │
└──────────────▲──────────────────────────────▲───────────────┘
               │ 帧呈现（payload_pool 或新 SHM 区）│ 输入事件
┌──────────────┴───────── Linux guest ─────────┴───────────────┐
│  Deaicup (egui + epaint + 软件光栅器)                         │
│    └─ 渲染到 SHM RGBA 缓冲，HLT park（与现有模型一致）          │
│    └─ HTTPS top.png 经 virtio-net + rustls 正常加载            │
└───────────────────────────────────────────────────────────────┘
```

#### 与现有基础设施的契合点（全部已验证）

- **IPC 通道**：`CODE/utsm-ipc/ipc_proto.h` 已定义双向 ring（64 条 × 256B）
  + VMCALL hypercall + park-and-resume 调度模型；exec 协议
  （`UTSM_MSG_EXEC_*`）和 Phase 3 文件传输（`UTSM_MSG_FILE_*` +
  payload_pool 分块）已在用。输入转发只需新增一对消息类型
  （如 `UTSM_MSG_INPUT_EVENT`），协议结构有成熟先例可循。
- **大块数据传输**：payload_pool 为 1015712 B（≈0.97 MiB），但
  800×600 RGBA 整帧 = 1 920 000 B ≈ 1.83 MiB，**单帧放不进现有 pool**。
  两个选项：
  1. **新增专用显示 SHM 区**（推荐）：当前 1MB 区布局是 packed 且 EPT 只映射
     该区；另开一个 GPA（如 0x05000000）映射 8 MiB，头部放
     宽高/stride/脏矩形序列号 + 双缓冲 RGBA，可支撑到 1920×1080。
     EPT 映射完全由 UTSM 控制，改动局部。
  2. 沿用 1MB pool + 脏矩形/行带分块：无需动 EPT，但 60fps × 每帧多次
     park-resume 的世界切换开销大，仅适合作为过渡。
- **调度模型**：现有模型是同步 park-and-resume（daemon 干完活 HLT，UTSM 的
  `linux_resume()` 返回）。GUI 应用可自然映射为**逐帧步进**：桌面事件循环
  每 tick（30/60Hz）把累积的输入事件写入 ring → `linux_resume()` →
  guest 内 egui 跑一帧（begin_frame/处理输入/update/光栅化到 SHM）→ HLT。
  无需引入 VMX preemption timer 等连续调度机制，与现有代码零冲突。
  将来若要多 Linux 应用常驻，再升级为抢占式调度。
- **网络**：virtio-net 数据路径已闭环，Arch rootfs + pacman 可用，意味着
  guest 内 TCP/DNS 已工作；ureq + rustls 为纯 Rust 进程内 TLS，不依赖系统
  组件，`top.png` 的 HTTPS 加载**无需任何改动**。

#### 需要新增的事

1. **guest 侧 egui 软件集成**（替换 eframe）：Deaicup 的 eframe 仅承担
   事件循环 + 后端，代码只有 ~80 行业务 UI。写一个自定义 runner：
   egui 0.27 + epaint + 软件光栅器（guest 内可直接用 crates.io 的
   tiny-skia，天然纯 CPU）+ SHM 输出 + ring 输入。约 300–500 行新代码，
   建议以 `deshab` feature flag 形式留在同一仓库，桌面开发仍用 eframe 跑。
2. **显示 SHM 区**：UTSM 侧 EPT 映射 + guest 内核补丁（utsm-ipc patch 已有
   先例）映射同一 GPA 到用户态（mmap）。
3. **Deshab 桌面"远程窗口"**：desktop.elf 新增一种窗口类型，内容区定期从
   显示 SHM blit（1.8MB memcpy 亚毫秒级），标题栏/关闭/最小化走现有
   窗口管理；窗口几何变化通过 ring 通知 guest 端 resize。
4. **输入协议**：PS/2 键鼠 → 结构化事件（x/y/按键/滚轮/修饰键）→ guest
   翻译成 `egui::RawInput`。
5. **生命周期**：窗口打开 → EXEC_REQUEST 启动 Deaicup；窗口关闭 →
   新消息通知 guest 端优雅退出。

#### 评估

- **工作量**：中等（显示 SHM + 输入转发 + guest runner + 桌面远程窗口，
  估计 2–4 周），且每一块都是现有已验证模式的增量。
- **渲染性能**：guest 内软件光栅 800×600 约数毫秒/帧；UTSM 侧 blit 为纯
  memcpy；逐帧步进延迟对工具类 GUI 完全可接受。
- **桌面集成度**：内容级集成（窗口内像素来自 guest），装饰/管理原生。
- **可维护性**：Deaicup 源码几乎不改，上游演进零成本同步；该通道同时是
  **未来所有 Linux GUI 应用的通用运行时**（WSLg 式复用）。
- **对 SAS-R0 侵入性**：低（新增 EPT 映射与一种桌面窗口类型，不动内核核心）。
- **风险**：中。唯一的新技术点是 epaint 软件光栅（guest 内用 tiny-skia
  已大幅降险）；其余皆已验证。

### 3.3 路径 C：deshab_ui.h 原生重写

#### 需要做的事

以 `CODE/tools/` 独立 .elf 模式（`dsk_entry` 入口，参照 `tool_apps_design.md`
ABI），用 deshab_ui.h 的 du_* 原语重写当前 102 行功能：

- 顶部横幅：top.png 需内嵌为静态 RGBA（DSK Logo 已有嵌入先例）或暂时省略
  —— **HTTP 拉取在 C 路径下不可行**（无 TCP/TLS）。
- 侧边导航：`du_draw_button` / `du_kate_panel` + 选中态手绘。
- 7 个 stub 页面：空白页，工作量近似为零。

#### 评估

- **工作量**：当前极小（功能对等约 1–2 天）。**但这是静态快照**：
  Deaicup 的 Tool/Game/LinuxWare/WindowsWare 页面注定会长出列表、滚动区、
  文本输入、图片网格等真实组件，deshab_ui.h 没有布局引擎与滚动容器，
  每加一个组件就是一次手工绘制 + 命中测试，长期成本线性增长。
- **与上游关系**：永久分叉。上游 Rust 仓库的每次改动都需要手工重新移植，
  Deaicup 本质上是"同一个产品的两套实现"。
- **渲染性能**：最好（零拷贝直绘 framebuffer）。
- **桌面集成度**：最好（原生 .elf 工具）。
- **对 SAS-R0 侵入性**：无。
- **定位**：适合作为**桌面壳层入口**（图标、启动器、About 页），不适合
  承担 Deaicup 本体。

## 4. 三路径对比矩阵

| 维度 | A 原生移植 | B Linux guest + IPC | C 原生重写 |
|---|---|---|---|
| 当前可见工作量 | 极大（2–4 人月+） | 中（2–4 周） | 极小（1–2 天） |
| 长期维护成本 | 高（std port 随 egui 演进） | 低（源码基本不改） | 高（组件库自建 + 双实现同步） |
| 技术风险 | 高 | 中（构件均已验证） | 低 |
| 渲染性能 | 好 | 中（memcpy blit，工具类足够） | 最好 |
| 桌面集成度 | 高（做成后） | 中（窗口内容级） | 最高 |
| 网络图片（top.png） | 不可行（无 TCP/TLS） | 原生可用（rustls 进程内 TLS） | 不可行 |
| 对 SAS-R0 侵入性 | 高 | 低 | 无 |
| 复用价值 | Rust 生态入口 | **通用 Linux GUI 运行时** | 无 |
| 上游代码同步 | 保留源码 | 保留源码 | 永久分叉 |

## 5. 推荐方案与分阶段实施

**推荐路径 B**，并以路径 C 的元素做桌面壳层入口（图标/启动器仍用原生 .elf
工具实现）。路径 A 仅在未来确有"原生 Rust 应用生态"战略需求时另行立项，
不为 Deaicup 启动。

### Phase 0：协议与集成设计（约 2–3 天）

- 在 `ipc_proto.h` 中新增：`UTSM_MSG_INPUT_EVENT`（键鼠事件载荷）、
  `UTSM_MSG_DISPLAY_*`（帧就绪/resize/关闭握手）消息类型。
- 确定显示 SHM 区方案：新 GPA（建议 0x05000000，8 MiB，双缓冲 + 脏矩形头）。
- Deaicup 仓库引入 `deshab` feature：默认仍用 eframe（Windows/Linux 桌面
  开发体验不变），feature 开启时切换到自定义 runner。

### Phase 1：guest 内 egui 软件渲染（约 1 周）

- 编写 Deaicup 的 deshab runner：egui 0.27 + epaint + tiny-skia 软件光栅
  → 显示 SHM RGBA；从 ring 读取输入事件构造 `egui::RawInput`。
- guest 内核侧把显示 SHM GPA mmap 到用户态（utsm-ipc patch 扩展）。
- 验证：guest 内独立运行，渲染固定测试帧写入 SHM，UTSM 侧 hexdump 校验。

### Phase 2：桌面远程窗口 + 输入转发（约 1–2 周）

- UTSM：EPT 映射显示 SHM；desktop.elf 新增远程窗口类型（内容区 blit SHM、
  几何变化通知 guest）。
- 输入转发：桌面焦点窗口的 PS/2 事件序列化为 `UTSM_MSG_INPUT_EVENT`。
- 逐帧步进调度：桌面 tick → 写输入 → `linux_resume()` → guest 渲染一帧
  → HLT → blit。
- 端到端验证：Deaicup 在 Deshab 桌面窗口内可点击导航、横幅 HTTPS 图片
  经 virtio-net 加载成功。

### Phase 3：完善（按需）

- 脏矩形增量 blit、窗口 resize 联动、关闭握手与进程回收、剪贴板互通。
- 多窗口/多 Linux GUI 应用（届时评估 VMX preemption timer 连续调度）。
- 性能优化：blit 与 guest 渲染重叠、帧率自适应（无输入无脏帧时降频）。

## 6. 风险与开放问题

1. **epaint 软件光栅质量**：egui 0.27 无官方 CPU 后端，tiny-skia 光栅化
   epaint 网格需自行实现三角形 + 图集 UV 采样（社区有 egui_tsk 等先例，
   风险可控），需在 Phase 1 早期做视觉验证（圆角/文字抗锯齿）。
2. **park-and-resume 帧率上限**：逐帧步进的世界切换成本决定帧率天花板，
   工具类 GUI 30fps 目标无虞；若未来要跑视频/游戏类负载，必须升级为
   抢占式 guest 调度。
3. **egui 版本演进**：Deaicup 锁定 0.27.2；自定义 runner 只依赖
   egui/epaint 公共 API（Context/RawInput/ClippedPrimitive），升级面小。
4. **横幅图片离线场景**：guest 无网络时 top.png 加载失败，egui 会显示
   占位——行为与桌面版一致，无需特殊处理。
