# Deshab 桌面环境架构设计

## 1. 概述

Deshab 桌面环境（Deshab Desktop Environment, DDE）是基于 UTSM/DSK 内核的图形桌面系统，
运行在 SAS-R0（单地址空间 Ring0）任务模型上，使用 "Sealed Arc" 视觉风格系统。

**设计原则**：
- **SAS-R0 适配**：无内存隔离，所有应用共享地址空间，信任模型为 Ring0 全信任
- **单屏幕协作**：初期采用"多窗口前台 + 桌面后台"模式，桌面管理器始终驻留
- **应用独立开发**：内置应用使用回调模式；未来工具应用为独立 .elf，通过 ELF 加载方式载入
- **帧缓冲直绘**：无 GPU 加速，所有渲染通过 du_context 直接写 framebuffer

**当前实现状态**：desktop.elf 已实现完整的桌面管理器框架，包括窗口管理、事件路由、
PS/2 鼠标初始化+轮询、光标 save/restore、任务栏、桌面图标和 3 个内置应用。

## 2. 系统架构

```
┌─────────────────────────────────────────────────────┐
│                    Framebuffer                       │
│  ┌─────────────────────────────────────────────┐    │
│  │           Desktop Manager (desktop.elf)      │    │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐    │    │
│  │  │ Window 1 │ │ Window 2 │ │ Window 3 │    │    │
│  │  │ (bash)   │ │ (editor) │ │ (calc)   │    │    │
│  │  └──────────┘ └──────────┘ └──────────┘    │    │
│  │                                             │    │
│  │  ┌─────────────────────────────────────────┐│    │
│  │  │              Taskbar (40px)              ││    │
│  │  └─────────────────────────────────────────┘│    │
│  └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────┘
```

## 3. 启动流程

```
UEFI → Limine → utsm.elf → DKM 驱动加载
  → deshab.elf (DSK) → spinner → firstInit.txt 检查
  → mouseInit.elf → netman.elf → [首次] FirstInit.elf
  → desktop.elf ← 桌面管理器
```

DSK 在 FirstInit 完成或 firstInit=1 时，加载 `desktop.elf`。
desktop.elf 通过 `dsk_entry(dsk_boot_context *ctx)` 入口接收系统上下文，
自行初始化 PS/2 鼠标、注册应用、进入主事件循环。

## 4. Desktop Manager 核心设计

### 4.1 窗口模型

```c
typedef struct desktop_window {
    int    id;                  /* 窗口 ID（自增分配） */
    int    x, y, w, h;         /* 窗口位置和尺寸 */
    int    visible;             /* 可见性 */
    int    focused;             /* 焦点 */
    int    minimized;           /* 最小化 */
    char   title[48];           /* 窗口标题 */

    /* 应用关联 */
    int    app_id;              /* 关联的应用 ID（g_apps 索引） */
    void  *app_state;           /* 应用私有状态（on_create 返回值） */
    int    closing;             /* 关闭中标记 */
    int    dirty;               /* 脏标记（需要重绘） */
} desktop_window;
```

**Z 序**：窗口按数组顺序排列，数组末尾的窗口在最上层。`win_bring_to_front()` 将窗口移到数组末尾。

**窗口限制**：最多 8 个窗口（`MAX_WINDOWS`）。

**固定地址分配**：当前内置应用使用固定地址分配状态结构，避免堆依赖：
- `0x4000000` — bash_state（终端）
- `0x5000000` — editor_state（编辑器）
- `0x6000000` — calc_state（计算器）

### 4.2 事件系统

```c
typedef enum {
    EV_MOUSE_MOVE = 1,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_KEY_DOWN,
    EV_KEY_UP,
} ev_type;

typedef struct {
    ev_type type;
    int mx, my;          /* 鼠标全局坐标 */
    int button;          /* 0=左, 1=右 */
    u8  scancode;
    int shift, ctrl, alt;
} desktop_event;
```

**事件路由**：
1. **鼠标事件** → 从后往前命中测试 → 找到目标窗口 → 检查关闭按钮/标题栏/客户区 → 路由到窗口回调
2. **键盘事件** → 当前焦点窗口的 `on_event` 回调
3. **任务栏点击** → 桌面管理器直接处理（快速启动/窗口按钮）
4. **桌面图标点击** → 桌面管理器处理（`launch_app()`）

**拖拽机制**：
- 鼠标按下 + 命中标题栏 → 进入拖拽模式（`g_dragging=1`）
- 拖拽期间鼠标移动 → 更新窗口位置（`w->x = mouse_x - offset`）
- 鼠标释放 → 退出拖拽模式

**关闭机制**：
- 鼠标点击窗口右上角红色 X 按钮（24×20 圆角矩形，DP_ERROR 底色）
- `win_destroy()` 调用应用的 `on_destroy`，从窗口数组移除，焦点转移到最后一个窗口

### 4.3 应用 ABI（回调模式）

```c
typedef struct app_ctx {
    du_context  *fb;              /* 全局帧缓冲渲染上下文 */
    desktop_win *win;             /* 关联窗口 */
    int          client_x, client_y, client_w, client_h;  /* 客户区坐标 */
    int          should_exit;     /* 退出标志 */
    desktop_block_read_fn block_read;   /* FAT32 块读取函数 */
    const void  *kernel_api;     /* DKM kernel_api 分发表 */
} app_ctx;

typedef void *(*app_create_fn)(app_ctx *ctx);
typedef void  (*app_event_fn)(void *state, app_ctx *ctx, desktop_event *ev);
typedef void  (*app_draw_fn)(void *state, app_ctx *ctx);
typedef void  (*app_destroy_fn)(void *state);

typedef struct {
    const char *name;             /* 应用标识名 */
    const char *display_name;     /* UI 显示名 */
    u32         default_w, default_h;  /* 默认窗口尺寸 */
    app_create_fn  on_create;
    app_event_fn   on_event;
    app_draw_fn    on_draw;
    app_destroy_fn on_destroy;
} app_descriptor;
```

**应用生命周期**：
1. 用户点击桌面图标或快速启动 → `launch_app(app_id)`
2. 创建窗口 → 构建 `app_ctx` → 调用 `on_create(ctx)` → 返回 `app_state`
3. 事件循环中：`on_event(state, ctx, ev)` 处理输入
4. 绘制时：`on_draw(state, ctx)` 渲染客户区
5. 关闭窗口 → `on_destroy(state)` 清理

**当前注册的应用**：

| app_id | name | display_name | 默认尺寸 | 状态 |
|--------|------|-------------|----------|------|
| 0 | bash | Terminal | 640×440 | 已实现 |
| 1 | editor | Editor | 600×450 | 已实现 |
| 2 | fileman | Files | 500×400 | 占位（无回调） |
| 3 | calc | Calculator | 280×400 | 已实现 |

### 4.4 任务栏 (Taskbar)

位置：屏幕底部，高度 40px。

布局：
```
┌──────────────────────────────────────────────────────────┐
│ [D] │ 🖥️  📝  📁  🧮 │           │ 12:34               │
│Start│  快速启动(4图标)   │  窗口按钮   │     RTC时钟          │
└──────────────────────────────────────────────────────────┘
```

分区：
- **Start 按钮**（48×32，DP_ABYSS_600 圆角矩形）：显示 "D" 字符（DS_DARK_ACCENT_LIGHT）
- **快速启动**（4 图标 × 40px）：按 app_id 顺序绘制程序化图标（终端/编辑器/文件夹/计算器）
- **窗口按钮区**：每个打开窗口一个 100×32 按钮，焦点窗口用 DS_DARK_ACCENT 底色，其余用 DP_ABYSS_600
- **RTC 时钟**（右侧 80px 区域）：通过 CMOS I/O（端口 0x70/0x71）读取 BCD 时分，转换为 HH:MM 格式

### 4.5 桌面图标

位置：桌面右侧垂直排列。

参数：
- 列宽 80px（`DESKTOP_ICON_W`），行高 72px（`DESKTOP_ICON_H`）
- 左边距 16px（`ICON_GUTTER_X`），上边距 16px（`ICON_GUTTER_Y`）
- 起始 X = `fb_width - 80 - 16`

图标：32×32 像素，程序化生成（无需位图文件），通过 `draw_icon_*` 函数绘制：
- **终端图标**：DP_ABYSS_700 圆角矩形 + DP_SEAL_300 的 >_ 符号 + DP_SEAL_500 边框
- **编辑器图标**：DP_ABYSS_700 圆角矩形 + DP_SEAL_500 文本行 + DP_SEAL_300 边框
- **文件夹图标**：DP_ARC_500 圆角矩形 + DP_ARC_300 文件夹形状 + DP_ARC_700 边框
- **计算器图标**：DP_ABYSS_700 圆角矩形 + DP_WARNING 按键 + DP_NEUTRAL_400 网格

标签：图标下方 36px 处，居中绘制应用 display_name，使用 DS_DARK_TEXT_PRIMARY 颜色。

### 4.6 窗口装饰

**标题栏**（28px，`TITLEBAR_H`）：
- 焦点窗口：DP_ABYSS_600 背景
- 非焦点窗口：DP_ABYSS_800 背景
- 标题文字：左侧 DU_SPACE_SM 偏移，DS_DARK_TEXT_PRIMARY 颜色
- 关闭按钮：右上角，窗口右边 -28px 处，24×20 圆角矩形，DP_ERROR (红色) 底色 + 白色 "X"

**边框**（2px，`BORDER_W`）：
- 焦点窗口：DS_DARK_BORDER（Seal Cyan 发光效果）
- 非焦点窗口：DS_DARK_BORDER
- 焦点发光：`du_rect_glow()` 3px 扩展范围，10px 衰减

**标题栏底线**：1px 分隔线，颜色同边框色。

**圆角**：窗口主体使用 `DU_RADIUS_LG` 圆角，关闭按钮使用 `DU_RADIUS_SM` 圆角。

### 4.7 应用加载模型

**方式一：回调式（当前实现）**

应用编译进 desktop.elf 内部，通过 `app_descriptor` 注册回调函数。
桌面管理器在事件循环中调用 `on_event()`/`on_draw()`，应用通过 `app_ctx` 访问系统服务。

特点：
- 应用与桌面管理器在同一 ELF 中，共享代码和全局变量
- 应用状态使用固定地址分配（避免堆依赖）
- 应用可以直接访问 g_fb 全局帧缓冲

**方式二：独立 ELF 加载式（未来实现）**

工具应用独立开发为单独 .elf 文件（如 `CODE/tools/bash/`），通过 DSK 的 FAT32 + ELF loader 加载。
独立工具实现 `dsk_entry(dsk_boot_context *ctx)` 入口点，接收与 DSK 相同的系统上下文。

未来桌面管理器将通过 ELF 加载方式载入工具 .elf，需要实现：
- ELF 加载到内存 + PIE 重定位
- 传入 dsk_boot_context（framebuffer、kernel_api、block_read 等）
- 返回机制（在 ELF 入口点保存返回地址）
- 独立工具的公共辅助代码（`CODE/tools/desktop_app.h`）

## 5. 渲染管线

```
redraw_all() 完整管线：
    1. 渐变背景（du_fill_bg_gradient: DP_ABYSS_900 → DP_ABYSS_800）
    2. 桌面图标（draw_desktop_icons）
    3. 窗口循环（从底到顶）：
       a. 应用 on_draw() 绘制客户区内容
       b. draw_window_frame() 绘制窗口装饰（在应用内容之上）
    4. 任务栏（draw_taskbar）
    5. 鼠标光标（cursor_save_bg + cursor_draw）
```

**渲染策略**：
- 每帧全屏重绘（无增量更新）
- 鼠标光标使用 save/restore 机制：移动前恢复旧位置背景，重绘后保存新位置背景
- 应用直接写入全局 framebuffer（无窗口后缓冲），窗口装饰在应用内容之上绘制
- 重绘触发：鼠标移动/点击、键盘事件、窗口状态变化

## 6. 像素格式

与现有 framebuffer 一致：32bpp ARGB（0xAARRGGBB），小端存储。
所有颜色值使用 deshab_ui.h 中的语义色令牌。

## 7. 鼠标光标

24×24 像素箭头光标：
- 热点在左上角 (0,0)
- 形状定义：`cursor_shape[24][24]` 位图数组（1=前景，0=透明）
- 前景色：DS_DARK_TEXT_PRIMARY
- save/restore 实现无闪烁移动：
  - `cursor_save_bg()`: 保存光标位置 24×24 像素到 `g_cursor_save[]`
  - `cursor_restore_bg()`: 恢复上次保存的像素
  - 每帧重绘时：先 restore 旧位置 → 全屏绘制 → save + draw 新位置
- 边界限制：光标不超出屏幕范围（减去 CURSOR_SIZE）

## 8. 输入处理

### 8.1 PS/2 鼠标

**初始化**（`ps2_mouse_init()`）：
1. 启用 AUX 设备（命令 0xA8 → 0x64）
2. 读取控制器配置字节（0x20 → 0x64），设置 bit1（AUX IRQ12）+ 清除 bit5（AUX clock）
3. 写回配置（0x60 → 0x64）
4. 复位鼠标（0xD4 → 0x64，0xFF → 0x60）
5. 启用 streaming（0xD4 → 0x64，0xF4 → 0x60）

**轮询**（`ps2_mouse_poll()`）：
- 读取端口 0x64 状态，检查 bit0（数据就绪）和 bit5（AUX 标记）
- 读取 0x60 数据，累积 3 字节组成一个鼠标包
- 校验 byte0 bit3 同步位
- 解析 X/Y 位移（带符号扩展）和左键状态（byte0 bit0）
- Y 轴翻转处理（鼠标 Y 向上为正，屏幕 Y 向下为正）
- 边界裁剪：光标限制在屏幕范围内

### 8.2 PS/2 键盘

- IRQ1 → scan code set 1
- 扩展键 E0 前缀追踪（`g_e0` 标记）
- Shift 修饰键追踪（0x2A/0x36 按下，0xAA/0xB6 释放）
- `scan_to_ascii()` 映射：normal[58] + shifted[58] 查找表
- 键盘事件发送到焦点窗口的 `on_event` 回调

**全局快捷键**（规划中）：
- Alt+Tab：窗口切换
- Alt+F4：关闭窗口

## 9. 文件系统接口

工具应用通过 `app_ctx.block_read` 访问 FAT32 文件系统。

`block_read` 函数类型：
```c
typedef int (*desktop_block_read_fn)(void *ctx, unsigned long long lba, unsigned int count, void *buffer);
```

从 `dkm_kernel_api + 0xA8` 偏移处获取函数指针。

暂不实现 VFS 系统调用——所有文件操作通过直接 FAT32 路径解析完成。

## 10. 网络接口

网络工具（curl、ping、browser）依赖 netman 已实现的：
- DHCP 客户端
- ARP 解析
- DNS 查询
- e1000/virtio_net TX/RX

工具应用通过 `kernel_api.net` 访问网络功能。

## 11. 内置应用详情

### 11.1 终端 (Bash)

- 状态结构：`bash_state` @ 0x4000000
- 终端缓冲：80 列 × 40 行（TERM_MAX_COLS × TERM_MAX_ROWS）
- 每格存储：字符(u8) + 前景色(u32)
- 输入缓冲：256 字节，支持插入模式和光标移动
- 提示符：`deshab# `
- 命令：help, clear, echo, version/ver, uname, date, about, reboot, halt, pwd, whoami, id
- 渲染：先绘制历史内容，再绘制当前输入行+光标
- 光标：2px 高下划线，DS_DARK_CURSOR 颜色
- 配色：DS_DARK_* 暗色主题

### 11.2 文本编辑器 (Editor)

- 状态结构：`editor_state` @ 0x5000000
- 文本缓冲：65536 字节（EDITOR_BUF_SIZE）
- 编辑操作：插入/删除字符、换行、方向键移动、Home/End
- 行号：3 位数字 + 分隔线（DS_DARK_TEXT_DIM 颜色）
- 自动滚动：光标超出可视区域时调整 scroll_y
- 状态栏：底部 20px，显示 L:行 C:列 + 修改标记(*)
- 光标：2px 宽竖线，DS_DARK_CURSOR 颜色
- 编辑区背景：DS_DARK_BG_SECONDARY
- 窗口标题：`Editor - untitled`

### 11.3 计算器 (Calculator)

- 状态结构：`calc_state` @ 0x6000000
- 运算：四则运算（+/-/×/÷），i64 精度
- 显示屏：圆角矩形，右对齐数字
- 按钮网格：4×5 布局（C/+/-/%/÷, 7/8/9/×, 4/5/6/-, 1/2/3/+, 0/./=）
- 按键颜色：功能键 DP_ABYSS_600，运算符 DS_DARK_ACCENT，等号 DP_SUCCESS
- 输入：键盘数字键 + 运算符，C 清零，Enter/= 计算

## 12. 分阶段实现计划

### Phase 1 — 桌面基础（已完成）
- ✅ Desktop Manager 核心框架
- ✅ 任务栏 + 时钟 + 桌面图标
- ✅ 窗口管理（创建/销毁/移动/焦点/拖拽）
- ✅ 事件路由系统
- ✅ PS/2 鼠标初始化 + 轮询
- ✅ 光标 save/restore
- ✅ 终端应用（Bash）
- ✅ 文本编辑器（Editor）
- ✅ 计算器（Calculator）

### Phase 2 — 文件操作与独立应用
- 工具应用独立开发框架（CODE/tools/ + desktop_app.h）
- FAT32 路径解析增强（目录遍历）
- 文件管理器（独立 .elf）
- 编辑器独立化（从内置 → 独立 tools/editor/）
- 文件打开器（关联 .txt → 编辑器）
- Ctrl+S 保存（需 FAT32 write API）

### Phase 3 — 网络
- TCP/IP 最小实现（基于 e1000/virtio_net）
- ping 工具（ICMP Echo）
- curl 工具（HTTP GET）

### Phase 4 — 高级应用
- 浏览器（HTTP + 简单 HTML 渲染）
- Bash 增强版（命令历史↑↓、ls、cat、Tab 补全）
- 更多系统工具

## 13. 目录结构

```
CODE/
  desktop/              桌面管理器（当前实现）
    main.c              主循环、事件路由、compositing、3个内置应用
    linker.ld           PIE 链接脚本
    MAKEFILE            构建脚本（输出 desktop.elf）

  tools/                独立工具应用（未来实现）
    desktop_app.h       公共辅助头文件
    bash/               独立终端
      main.c
      MAKEFILE
      linker.ld
    editor/             独立文本编辑器
      main.c
      MAKEFILE
      linker.ld
    fileman/            文件管理器
      main.c
      MAKEFILE
      linker.ld
    browser/            浏览器
      main.c
      MAKEFILE
      linker.ld
    curl/               HTTP 客户端
      main.c
      MAKEFILE
      linker.ld
    ping/               ICMP 工具
      main.c
      MAKEFILE
      linker.ld
```

## 14. 构建与打包

### 当前构建

desktop.elf 编译输出到 `SYSTEM/system/deshab64/desktop.elf`。

编译参数（与 DSK / shell / FirstInit 一致）：
```
clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin
      -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-sse2
      -mno-mmx -msoft-float -m64 -Wall -Wextra -I../UTSM/include -O2
ld.lld -nostdlib -static -pie -z max-page-size=0x1000 -T linker.ld
```

### 未来工具应用构建

独立工具 .elf 将输出到 `SYSTEM/system/deshab64/tools/`，构建集成：
- 每个工具有独立 MAKEFILE（与 desktop 相同模式）
- build.ps1 中添加工具构建步骤
- mkfat32.c 中添加工具 .elf 入口

```
SYSTEM/system/deshab64/
  desktop.elf           ← 桌面管理器
  tools/
    bash.elf            ← 独立终端
    editor.elf          ← 独立文本编辑器
    fileman.elf         ← 文件管理器
    browser.elf         ← 浏览器
    curl.elf            ← HTTP 客户端
    ping.elf            ← ICMP 工具
```

DSK 启动流程：
- 首次启动：mouseInit → netman → FirstInit → desktop
- 非首次启动：mouseInit → netman → desktop
