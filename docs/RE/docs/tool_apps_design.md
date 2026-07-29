# Deshab 工具应用设计文档

## 1. 概述

Deshab 工具应用是独立开发、独立编译的 .elf 可执行文件，运行在桌面管理器（Desktop Manager）环境中。
每个工具在 `CODE/tools/{name}/` 下拥有独立的 `main.c` + `MAKEFILE` + `linker.ld`，
通过 DSK 的 FAT32 + ELF loader 加载，接收 `dsk_boot_context` 系统上下文。

**核心约束**：
- 所有应用运行在 Ring0，单地址空间，无内存隔离
- 独立工具通过 `dsk_entry(dsk_boot_context *ctx)` 入口点启动
- 工具可以直接访问 framebuffer、PS/2 输入、kernel_api
- 网络工具依赖 kernel_api.net，文件工具依赖 FAT32 block_read
- 公共辅助代码通过 `CODE/tools/desktop_app.h` 共享

**开发模式演进**：
- **当前阶段**：3 个内置应用（Bash/Editor/Calculator）使用回调模式编译在 desktop.elf 内
- **目标阶段**：工具应用独立开发为单独 .elf，桌面管理器通过 ELF 加载方式载入

## 2. 应用 ABI

### 2.1 独立工具入口

独立工具 .elf 实现以下入口点：

```c
__attribute__((visibility("default")))
void dsk_entry(const dsk_boot_context *ctx);
```

工具接收完整的 `dsk_boot_context`，包含：

```c
typedef struct dsk_boot_context {
    u64 magic;                    /* 0x44534B31424F4F54 ("DSK1BOOT") */
    u32 abi_version;              /* 1 */
    u32 size;

    u64 flags;
    u64 hhdm_offset;
    u64 rsdp_address;

    u64 framebuffer_address;      /* 帧缓冲物理地址 (+ HHDM) */
    u64 framebuffer_width;
    u64 framebuffer_height;
    u64 framebuffer_pitch;
    u32 framebuffer_bpp;

    u64 boot_modules_response;
    u64 dkm_kernel_api;           /* DKM kernel_api 分发表 */
    u64 dkm_driver_table;
    u64 dkm_driver_count;

    u64 utsm_state;
    u64 drr_state;
    u64 memory_map;
    u64 memory_map_count;
    u64 memory_map_entry_size;
    u64 kernel_stack_top;
} dsk_boot_context;
```

### 2.2 公共辅助头文件

`CODE/tools/desktop_app.h` 提供所有工具共享的定义和辅助函数：

```c
/* 类型定义 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

/* COM1 串口日志 */
#define COM1 0x3F8
static inline void outb(u16 port, u8 value);
static inline u8 inb(u16 port);
void sputc(char c);
void swrite(const char *s);
void slog(const char *s);

/* PS/2 键盘轮询 */
int ps2kbd_poll(u8 *scancode);    /* 返回 1=有数据 */

/* PS/2 鼠标轮询 */
int ps2mouse_poll(int *dx, int *dy, int *btn);  /* 返回 1=有数据包 */

/* scan code set 1 → ASCII 映射 */
char scan_to_ascii(u8 sc, int shift);

/* 帧缓冲绘制辅助 */
void fb_init(u64 addr, u64 w, u64 h, u64 pitch);
void fb_pixel(int x, int y, u32 color);
void fb_rect(int x, int y, int w, int h, u32 color);
void fb_string(const char *s, int x, int y, u32 fg, u32 bg, int step);

/* FAT32 文件读取辅助 */
typedef int (*block_read_fn)(u32 index, u64 lba, u32 count, void *buf);
int fat32_read_file(block_read_fn br, const char *path, u8 **data, u32 *size);

/* 窗口概念（标题栏+关闭按钮+客户区） */
#define WIN_TITLEBAR_H  28
#define WIN_BORDER_W    2
void win_draw_frame(int x, int y, int w, int h, const char *title, int focused);
int  win_hit_close(int wx, int wy, int ww, int wh, int mx, int my);
```

### 2.3 当前内置应用 ABI（回调模式）

当前 3 个内置应用编译在 desktop.elf 内，使用回调模式：

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
```

### 2.4 应用生命周期

**回调式（当前）**：
```
1. 用户点击图标/快速启动按钮
2. 桌面管理器创建窗口
3. 桌面调用 app->on_create(ctx) → 返回 app_state
4. 事件循环：桌面调用 app->on_event(app_state, ctx, ev)
5. 绘制：桌面调用 app->on_draw(app_state, ctx)
6. 用户关闭窗口 → 桌面调用 app->on_destroy(app_state)
7. 桌面销毁窗口，回收资源
```

**独立 ELF 式（未来）**：
```
1. 桌面管理器通过 FAT32 读取工具 .elf
2. PIE ELF loader 加载 + 重定位
3. 调用 dsk_entry(ctx)
4. 工具自行接管输入/渲染/主循环
5. 工具返回 → 桌面管理器恢复控制
```

## 3. 工具应用详细设计

### 3.1 终端 (Bash) — CODE/tools/bash/

**状态**：内置版本已实现（回调模式）；独立版本待开发

**功能**：
- 接收 dsk_boot_context，全屏暗色终端
- PS/2 键盘输入，命令历史（↑↓键）
- 内置命令：help, clear, echo, version/ver, uname, date, about, reboot, halt, pwd, whoami, id, ls, cat
- Sealed Arc 暗色主题（DS_DARK_* 色系）

**UI 设计**：
```
┌─ deshab# ────────────────────────────[✕]─┐
│ deshab# echo "Hello, Deshab!"            │
│ Hello, Deshab!                           │
│ deshab# _                                │
│                                          │
│                                          │
│                                          │
├──────────────────────────────────────────┤
│ /                          12:34         │
└──────────────────────────────────────────┘
```

**独立版本结构**：
- 标题栏：`deshab#` 标题 + 右侧红色 X 关闭按钮
- 主区域：暗色终端输出区 + 当前输入行 + 闪烁光标
- 状态栏（底部 20px）：左侧当前目录 `/`，右侧 RTC 时间

**数据结构**：
```c
typedef struct {
    u8  ch[TERM_MAX_CHARS];       /* 字符缓冲 */
    u32 fg[TERM_MAX_CHARS];       /* 前景色缓冲 */
    int cols, rows;               /* 终端尺寸 */
    int cur_col, cur_row;         /* 当前光标位置 */
    char input_buf[256];          /* 当前输入行 */
    int input_len, input_cursor;  /* 输入长度和光标 */
    int prompt_len;               /* 提示符长度 */
    char history[16][256];        /* 命令历史 */
    int history_count;            /* 历史条数 */
    int history_pos;              /* 历史浏览位置 */
} bash_state;
```

**命令实现**：

| 命令 | 功能 | 依赖 |
|------|------|------|
| help | 显示可用命令列表 | 无 |
| clear | 清屏 | 无 |
| echo | 回显参数 | 无 |
| version / ver | 显示版本 "Deshab OS v0.1.0" | 无 |
| uname | 显示系统名 "Deshab" | 无 |
| date | 显示 RTC 日期时间 | CMOS |
| about | 显示系统信息 | 无 |
| reboot | 重启（0x64→0xFE） | 键盘控制器 |
| halt | 停机（HLT） | 无 |
| pwd | 显示当前目录 "/" | 无 |
| whoami | 显示 "root" | 无 |
| id | 显示 "uid=0(root) gid=0(root)" | 无 |
| ls | 列出目录内容 | FAT32 增强遍历 |
| cat | 显示文件内容 | FAT32 读取 |

---

### 3.2 文本编辑器 (Editor) — CODE/tools/editor/

**状态**：内置版本已实现（回调模式）；独立版本待开发

**功能**：
- 全屏编辑器，行号 + 编辑区 + 状态栏
- FAT32 文件打开（通过 kernel_api block_read）
- 编辑操作：插入/删除字符、换行、光标移动（方向键/Home/End）
- PageUp/PageDown 翻页
- 文本选择：Shift+方向键
- Ctrl+S 保存（需 FAT32 write API）
- 状态栏：行:列、文件名、修改标记

**UI 设计**：
```
┌─ Editor — README.TXT ───────────────[✕]─┐
│  1│ # Deshab OS                         │
│  2│ 单地址空间 Ring0 内核实验系统         │
│  3│                                      │
│  4│ ## 概述                              │
│  5│ Deshab 是一个...█                    │
│  │ │                                     │
│  │ │                                     │
├───┴──────────────────────────────────────┤
│ L:5  C:15  README.TXT  *                 │
└──────────────────────────────────────────┘
```

**数据结构**：
```c
typedef struct {
    char   text[EDITOR_BUF_SIZE];  /* 文本缓冲 (64KB) */
    int    text_len;                /* 文本长度 */
    int    cursor_pos;              /* 光标位置（字节偏移） */
    int    scroll_y;                /* 垂直滚动偏移（行数） */
    int    cursor_line;             /* 光标行号 */
    int    cursor_col;              /* 光标列号 */
    int    sel_start, sel_end;      /* 选择范围 */
    int    modified;                /* 修改标记 */
    char   filename[128];           /* 当前文件名 */
} editor_state;
```

**渲染细节**：
- 行号：3 位数字宽度（`DS_DARK_TEXT_DIM` 颜色）+ 1px 分隔线（`DS_DARK_DIVIDER`）
- 编辑区背景：`DS_DARK_BG_SECONDARY`
- 光标：2px 宽竖线，`DS_DARK_CURSOR` 颜色
- 状态栏：底部 20px，`DP_ABYSS_800` 背景，显示 `L:行 C:列 文件名 *修改标记`

---

### 3.3 文件管理器 (FileManager) — CODE/tools/fileman/

**状态**：占位（当前 desktop.elf 中 fileman 无回调）；独立版本待开发

**功能**：
- 双栏布局：左侧目录树、右侧文件列表
- FAT32 目录遍历（需增强到子目录）
- 文件图标区分：目录/ELF/TXT/CONF
- 双击打开：.txt/.conf → 编辑器，.elf → 执行
- 路径栏 + 返回按钮

**UI 设计**：
```
┌─ 文件管理器 ────────────────────────[✕]─┐
│ [←] /system/deshab64/                    │
├──────────┬───────────────────────────────┤
│ /        │ 📁 tools/          DIR        │
│ /system  │ 📄 desktop.elf    53KB  ELF   │
│ /driver  │ 📄 editor.elf     28KB  ELF   │
│          │ 📄 deshab.elf    131KB  ELF   │
│          │ 📄 README.TXT      2KB  TXT   │
│          │ 📄 user.conf       1KB  CONF  │
├──────────┴───────────────────────────────┤
│ 6 项                                     │
└──────────────────────────────────────────┘
```

**文件图标颜色**：

| 类型 | 图标 | 主色调 |
|------|------|--------|
| 目录 | 📁 | DP_ARC_500 (紫) |
| ELF | 📄 + 可执行标记 | DP_SEAL_300 (青) |
| TXT | 📄 + 文本标记 | DP_NEUTRAL_300 (灰) |
| CONF | 📄 + 齿轮标记 | DP_WARNING (琥珀) |

**依赖**：FAT32 子目录遍历（当前 DSK 仅支持根目录读取，需增强）

---

### 3.4 浏览器 (Browser) — CODE/tools/browser/

**状态**：Phase 4 实现（依赖 TCP/IP + HTTP + HTML 渲染）

**功能**：
- 地址栏 + 导航按钮（后退/前进/刷新）
- HTTP GET 请求（通过 kernel_api.net）
- 简单 HTML 渲染：标题、段落、链接、列表
- 超链接点击导航
- 历史记录（后退/前进栈）

**UI 设计**：
```
┌─ 浏览器 ────────────────────────────[✕]─┐
│ [←][→][↻]  [http://example.com    ][Go] │
├──────────────────────────────────────────┤
│ Example Domain                           │
│                                          │
│ This domain is for use in illustrative   │
│ examples in documents. You may use this  │
│ domain in literature...                  │
│                                          │
│ More information...                      │
└──────────────────────────────────────────┘
```

**依赖**：
- TCP/IP 栈（需实现 TCP 连接、发送/接收、拥塞控制）
- HTTP/1.1 客户端（GET 请求、响应解析）
- DNS 解析（netman 已实现）
- HTML 解析器（简化版，仅处理常见标签）

---

### 3.5 HTTP 客户端 (curl) — CODE/tools/curl/

**状态**：Phase 3 实现（依赖 TCP/IP 最小实现）

**功能**：
- 命令行界面运行（终端风格）
- HTTP GET/HEAD 请求
- URL 解析（host:port/path）
- 响应头显示（`-i` 选项）
- 超时处理

**命令行**：
```
curl http://example.com
curl -i http://example.com
curl --head http://example.com
```

**UI 设计**：
```
┌─ curl ──────────────────────────────[✕]─┐
│ > curl http://example.com                │
│ HTTP/1.1 200 OK                          │
│ Content-Type: text/html                  │
│                                          │
│ <!doctype html>                          │
│ <html>                                   │
│ ...                                      │
└──────────────────────────────────────────┘
```

**依赖**：TCP/IP 栈 + DNS 解析

**实现方式**：作为窗口化终端风格应用运行，输出 HTTP 响应内容。

---

### 3.6 ICMP 工具 (ping) — CODE/tools/ping/

**状态**：Phase 3 实现（依赖 ICMP + ARP）

**功能**：
- 发送 ICMP Echo Request
- 接收 ICMP Echo Reply
- RTT 计算（基于 PIT 已校准计时器）
- 统计信息显示（发送/接收/丢包率/平均 RTT）

**命令行**：
```
ping 192.168.1.1
ping -c 4 10.0.2.2
```

**UI 设计**：
```
┌─ ping ──────────────────────────────[✕]─┐
│ PING 192.168.1.1                         │
│ 64 bytes: seq=0 time=2ms                 │
│ 64 bytes: seq=1 time=1ms                 │
│ 64 bytes: seq=2 time=3ms                 │
│                                          │
│ --- 192.168.1.1 ping statistics ---      │
│ 3 packets transmitted, 3 received, 0%    │
│ RTT min/avg/max = 1/2/3 ms              │
└──────────────────────────────────────────┘
```

**依赖**：
- ICMP 协议实现（需通过 kernel_api.net 发送原始 IP 包）
- ARP 解析（netman 已实现）
- 计时器（PIT 已校准）

---

### 3.7 更多应用（规划中）

| 应用 | 描述 | Phase | 依赖 |
|------|------|-------|------|
| System Monitor | CPU/内存/网络状态 | 2 | kernel_api |
| Paint | 简单绘图工具 | 2 | 无 |
| Settings | 系统偏好设置 | 2 | FAT32 write |
| Screenshot | 屏幕截图保存 | 3 | FAT32 write |
| Music Player | PC 喇叭/AC97 音频 | 4 | 音频驱动 |

## 4. 依赖矩阵

```
           FAT32 Read | FAT32 Write | TCP/IP | ICMP | DNS | ARP | PS/2输入
bash          ✓       |             |        |      |     |     |    ✓
editor        ✓       |    (保存)    |        |      |     |     |    ✓
fileman       ✓       |             |        |      |     |     |    ✓
browser       ✓       |             |   ✓    |      |  ✓  |  ✓  |    ✓
curl          ✓       |             |   ✓    |      |  ✓  |  ✓  |    ✓
ping          ✓       |             |        |  ✓   |     |  ✓  |    ✓
calculator    |        |             |        |      |     |     |    ✓
```

## 5. 应用图标设计

每个应用使用 32×32 像素图标，程序化生成（C 函数绘制，无需位图文件）。

| 应用 | 图标描述 | 主色调 | 绘制函数 |
|------|----------|--------|----------|
| bash | 终端窗口 >_ | DP_SEAL_300 (亮青) | `draw_icon_terminal()` |
| editor | 笔记本+文本行 | DP_SEAL_500 (青) | `draw_icon_editor()` |
| fileman | 文件夹 | DP_ARC_500 (紫) | `draw_icon_folder()` |
| calculator | 计算器按键 | DP_WARNING (琥珀) | `draw_icon_calc()` |
| browser | 地球/指南针 | DP_SEAL_300 (亮青) | 待实现 |
| curl | 箭头+网络 | DP_SEAL_700 (深青) | 待实现 |
| ping | 信号波 | DP_SUCCESS (绿) | 待实现 |

**图标绘制规范**：
- 圆角矩形背景（6px 圆角，`DU_RADIUS_SM`）
- 1px 描边（使用边框色）
- 前景图形使用应用主色调
- 背景色：DP_ABYSS_700

## 6. 构建集成

### 6.1 独立工具 MAKEFILE

每个工具有独立的 MAKEFILE（与 desktop.elf 相同模式）：

```makefile
# tools/bash/MAKEFILE 示例
CC := clang
LD := ld.lld

ROOT := ../../..
OUT := $(ROOT)/SYSTEM/system/deshab64/tools/bash.elf

CFLAGS := -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin \
          -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-sse2 \
          -mno-mmx -msoft-float -m64 -Wall -Wextra -I.. -I../../UTSM/include -O2
LDFLAGS := -nostdlib -static -pie -z max-page-size=0x1000 -T linker.ld

C_SOURCES := main.c
OBJECTS := $(C_SOURCES:.c=.o)

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJECTS) linker.ld
	@if not exist "$(ROOT)\SYSTEM\system\deshab64\tools" mkdir "$(ROOT)\SYSTEM\system\deshab64\tools"
	$(LD) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
```

### 6.2 build.ps1 集成

在 `build.ps1` 中添加工具构建步骤：

```powershell
# 构建工具应用
$tools = @("bash", "editor", "fileman", "curl", "ping", "browser")
foreach ($tool in $tools) {
    Push-Location "CODE/tools/$tool"
    & make -f MAKEFILE
    Pop-Location
}
```

### 6.3 FAT32 镜像更新

`mkfat32.c` 中添加工具 .elf 入口：

```
SYSTEM/system/deshab64/tools/bash.elf
SYSTEM/system/deshab64/tools/editor.elf
SYSTEM/system/deshab64/tools/fileman.elf
SYSTEM/system/deshab64/tools/curl.elf
SYSTEM/system/deshab64/tools/ping.elf
SYSTEM/system/deshab64/tools/browser.elf
```

## 7. 测试策略

- **单元测试**：每个应用的事件处理回调可用模拟事件测试
- **QEMU 集成测试**：完整的启动→桌面→打开应用→操作→关闭流程
- **串口日志**：所有应用通过 COM1 输出调试信息（`[bash]`、`[editor]` 等前缀）
- **FAT32 验证**：使用 `driver/test.fat32` 测试镜像验证文件读取功能
- **独立工具验证**：DSK 直接加载工具 .elf，检查 dsk_entry 入口调用和上下文传递
