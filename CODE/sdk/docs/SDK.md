# Deshab SDK 使用文档

## 1. 概述

Deshab SDK 是 Deshab 操作系统的应用与驱动统一开发工具包。在 SAS-R0 单地址空间 Ring0 模型下，应用（`.elf`）与驱动（`.drv`）共享同一套基础类型、ABI 契约和工具函数，仅链接形式不同：

- **应用**：PIE ELF，由 DSK 加载器加载执行，入口 `dsk_entry`。
- **驱动**：ET_REL 可重定位对象，由 DKM loader 装载，导出 `driver_desc` / `driver_init` / `driver_exit`。

所有 SDK 头文件均为 `static inline` 实现，无库依赖，包含即用。

### 目录结构

```
CODE/sdk/
├── include/deshab/          SDK 头文件（单一包含入口）
│   ├── deshab.h             伞头：基础层 + ABI 层 + 应用层（不含 fat32/net）
│   ├── types.h              基础类型 u8/u16/u32/u64/i8/i16/i32/i64
│   ├── portio.h             端口 I/O（dsb_inb/dsb_outb/inw/outw/inl/outl）
│   ├── serial.h             COM1 串口日志（dsb_slog/dsb_swrite/dsb_sputc）
│   ├── tsc.h                TSC 校准与延时（dsb_tsc_calibrate/dsb_delay_ms）
│   ├── string.h             基础内存操作（dsb_memset/dsb_memcpy/dsb_strlen）
│   ├── kernel_api.h         dkm_kernel_api ABI 结构体（字段顺序不可变）
│   ├── boot_context.h       dsk_boot_context 启动上下文与入口契约
│   ├── driver.h             DKM 驱动描述符与状态机定义
│   ├── block.h              block provider 便捷读写层
│   ├── app.h                dsb_app_context 应用上下文初始化
│   ├── fb.h                 帧缓冲绘制原语（pixel/fill_rect/blend/rounded_rect）
│   ├── font.h               ASCII 位图字体渲染（dsb_draw_char/dsb_draw_string）
│   ├── cursor.h             鼠标光标 save/restore/draw
│   ├── input.h              PS/2 键盘扫描码转 ASCII + 鼠标初始化与轮询
│   ├── ui.h                 色板常量 + 标题栏/状态栏装饰 + LINUXAPP.CNF 解析
│   ├── rtc.h                CMOS RTC 时分读取
│   ├── fat32.h              FAT32 读写库 + LFN 长文件名（含 256KB+4KB+256KB BSS）
│   └── net.h                用户态网络协议栈 ARP/ICMP/DNS/TCP（含 64KB BSS）
├── scaffold/                脚手架模板
│   ├── new-module.ps1       一键创建新应用/驱动模块
│   ├── app/                 应用模板（main.c / MAKEFILE / linker.ld）
│   └── driver/              驱动模板（main.c）
├── examples/                示例代码
│   ├── hello-app/           最小应用示例
│   └── hello-driver/        最小驱动示例
└── docs/
    └── SDK.md               本文档
```

---

## 2. 快速开始

### 2.1 使用脚手架创建新模块

```powershell
# 创建应用（默认输出到 CODE/tools/<Name>，产物到 SYSTEM/system/deshab64/tools/<Name>.elf）
cd d:\Code\Deshab\CODE\sdk\scaffold
.\new-module.ps1 -Name myapp

# 创建驱动（默认输出到 CODE/DKM/<Name>）
.\new-module.ps1 -Name mydriver -Type driver -Class DKM_CLASS_NET -Stage 3
```

脚本自动计算相对路径并替换模板占位符，生成 `main.c` / `MAKEFILE` / `linker.ld`（应用）或 `main.c`（驱动）。

可选参数：

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-Name` | 模块名（必填） | — |
| `-Type` | `app` 或 `driver` | `app` |
| `-OutDir` | 输出目录 | app→`CODE/tools/$Name`，driver→`CODE/DKM/$Name` |
| `-Class` | 驱动类别 `DKM_CLASS_*` | `DKM_CLASS_MISC` |
| `-Stage` | 驱动加载阶段 0-3 | `3` |

### 2.2 构建命令

**应用**（需要 clang + ld.lld + make，make 需 `sh.exe` 在 PATH 中）：

```powershell
cd CODE\tools\myapp        # 或 CODE\sdk\examples\hello-app
make                        # 构建到 SYSTEM/system/deshab64/tools/myapp.elf
make clean                  # 清理
```

**驱动**（两步：编译 + 链接为可重定位对象）：

```powershell
cd CODE\DKM\mydriver
clang -target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin `
      -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel `
      -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 `
      -Wall -Wextra -I..\sdk\include -O2 -c main.c -o mydriver.o
ld.lld -r -o mydriver.drv mydriver.o
```

> 提示：项目根目录的 `build.ps1` 会批量构建所有驱动并打包镜像，日常开发只需 `.\build.bat`。

---

## 3. 应用开发

### 3.1 入口契约

DSK 加载 `.elf` 后调用 `dsk_entry(dsk_boot_context *ctx)`。应用必须：

1. **校验 magic**：`ctx->magic == DSK_BOOT_MAGIC`（`0x44534B31424F4F54`，即 `"DSK1BOOT"`）。
2. **初始化上下文**：调用 `dsb_app_init(&ac, ctx)` 提取 framebuffer、block、kernel_api。
3. （可选）校验 `ctx->abi_version == DSK_BOOT_ABI_VERSION`。

```c
#include "../../../firstInit/ascii_bitmaps.c"  /* 提供字体位图数据 */
#include "deshab/deshab.h"
#include "deshab/font.h"                       /* 需先有位图数据源 */

void dsk_entry(const dsk_boot_context *ctx) {
    if (ctx->magic != DSK_BOOT_MAGIC) return;
    dsb_app_context ac;
    dsb_app_init(&ac, ctx);
    /* ... 你的逻辑 ... */
}
```

### 3.2 dsb_app_context

`dsb_app_init` 从 boot context 一次性提取应用常用数据：

| 字段 | 类型 | 来源 |
|------|------|------|
| `fb` | `u32*` | `ctx->framebuffer_address`（32bpp BGRA） |
| `fb_w` | `u64` | `ctx->framebuffer_width` |
| `fb_h` | `u64` | `ctx->framebuffer_height` |
| `fb_pitch` | `u64` | `ctx->framebuffer_pitch` |
| `block` | `dsb_block` | 从 `ctx->dkm_kernel_api + DSB_KAPI_OFF_BLOCK` 解析 |
| `api` | `const dkm_kernel_api*` | `ctx->dkm_kernel_api` |

### 3.3 绘制 API

所有绘制函数以 `dsb_app_context*` 为首参，假设 32bpp BGRA 帧缓冲：

| 函数 | 说明 |
|------|------|
| `dsb_fill_bg(ac, color)` | 全屏填充 |
| `dsb_fill_rect(ac, x, y, w, h, color)` | 矩形填充 |
| `dsb_fill_rounded_rect(ac, x, y, w, h, color, radius)` | 圆角矩形填充 |
| `dsb_rect_outline(ac, x, y, w, h, color, radius)` | 矩形描边 |
| `dsb_pixel(ac, x, y, color)` | 单像素（自动越界保护） |
| `dsb_pixel_read(ac, x, y)` | 读单像素 |
| `dsb_blend(bg, fg, alpha)` | Alpha 混合（0-255） |
| `dsb_draw_char(ac, ch, x, y, fg, bg)` | 单字符（8 级灰度抗锯齿） |
| `dsb_draw_string(ac, s, x, y, fg, bg, step)` | 字符串（step=字间距，12 为常规） |
| `dsb_draw_titlebar(ac, title, w)` | 标题栏（含关闭按钮 + 分隔线） |
| `dsb_draw_statusbar(ac, text, w, h)` | 底部状态栏 |

**色板常量**（`ui.h`）：

| 常量 | 值 | 用途 |
|------|----|------|
| `DSB_BG_PRIMARY` | `0xFF0A1428` | 主背景（深蓝） |
| `DSB_BG_SECONDARY` | `0xFF0F1E38` | 次背景（标题栏/状态栏） |
| `DSB_TEXT_PRIMARY` | `0xFFE8E8F0` | 主文字 |
| `DSB_TEXT_DIM` | `0xFF6A6A88` | 暗淡文字 |
| `DSB_ACCENT` | `0xFF00A8CC` | 强调色 |
| `DSB_ERROR` | `0xFFFF4466` | 错误/关闭按钮 |
| `DSB_SUCCESS` | `0xFF40C880` | 成功 |
| `DSB_WARNING` | `0xFFF0A030` | 警告 |

**字体前置条件**：`font.h` 依赖 `g_ascii[95]` / `g_ascii_w` / `g_ascii_h` 三个符号，必须先 `#include "ascii_bitmaps.c"`（或自定义位图源）再包含 `font.h`。

### 3.4 输入 API

**键盘**（轮询模式，scan code set 1）：

```c
u8 st = dsb_inb(0x64);
if (st & 1) {
    u8 sc = dsb_inb(0x60);
    char ch = dsb_scan_to_ascii(sc, shift_pressed);
    /* sc == 1 为 Esc，sc == 0x36/0x2A 为 Shift */
}
```

**鼠标**（PS/2 轮询模式）：

```c
dsb_mouse mouse;
dsb_cursor cursor;
dsb_mouse_init();                        /* 初始化 AUX 端口 + 流模式 */
dsb_cursor_init(&cursor, ac.fb_w, ac.fb_h);

/* 主循环中 */
if (dsb_mouse_poll(&mouse, &cursor, ac.fb_w, ac.fb_h)) {
    /* cursor.mx / cursor.my / cursor.btn 已更新 */
    dsb_cursor_save(&ac, &cursor);       /* 先恢复旧位置背景 */
    dsb_cursor_restore(&ac, &cursor);    /* （需先 save 过） */
    dsb_cursor_draw(&ac, &cursor, DSB_CURSOR_COLOR);
}
```

> 注意：`dsb_mouse_poll` 先检查 AUX 位（`0x20`）再读数据，避免吞掉键盘扫描码。

### 3.5 FAT32 与网络（按需引入）

`fat32.h` 和 `net.h` 因含较大 static 缓冲（分别约 516KB 和 64KB BSS），**不在** `deshab.h` 伞头中默认包含，需显式 `#include`：

```c
#include "deshab/deshab.h"
#include "deshab/fat32.h"   /* 需要文件读写时 */
#include "deshab/net.h"     /* 需要网络时 */
```

**FAT32 用法**：

```c
/* 从 block API 初始化（block_read/write 来自 kernel_api） */
dsb_block blk;
dsb_block_init(&blk, ctx->dkm_kernel_api);
f32_init(blk.read, blk.write);

/* 读根目录文件 */
u8 *data; u32 size;
char name11[11];
f32_name_to_83("CONFIG.TXT", name11);
if (f32_read_root_file(name11, &data, &size) == 0) {
    /* data 指向 f32_data 内部缓冲，size 为字节数 */
}

/* 写文件 */
f32_write_root_file(name11, my_data, my_size);

/* 长路径 LFN 读取（支持子目录） */
if (f32_read_path_lfn("system/deshab64/FUCK", &data, &size) == 0) { ... }
```

**网络用法**（QEMU slirp 默认拓扑）：

```c
ns_init(ctx->dkm_kernel_api);           /* 绑定第一个有线 tx/rx ready 设备 */
ns_apply_lease_from_ctx((u64)ctx);      /* 应用 netman DHCP 租约（若有） */

u32 ip;
if (ns_dns_resolve("example.com", &ip) == 0) {
    u32 rtt; u8 ttl;
    if (ns_ping(ip, 1, 3000, &rtt, &ttl) == 0) { /* ping 成功 */ }
}

/* TCP client */
if (ns_tcp_connect(ip, 80, 5000) == 0) {
    ns_tcp_send((const u8*)"GET / HTTP/1.0\r\n\r\n", 18);
    u8 buf[4096];
    int n = ns_tcp_recv(buf, sizeof(buf), 5000);
    ns_tcp_close();
}
```

---

## 4. 驱动开发

### 4.1 三符号契约

DKM 驱动为 ET_REL 可重定位对象（`.drv`），必须导出三个符号：

| 符号 | 类型 | 说明 |
|------|------|------|
| `driver_desc` | `const dkm_driver_desc` | 描述符（magic/类别/阶段/依赖/提供） |
| `driver_init` | `int (*)(const dkm_kernel_api *api, dkm_driver_handle *handle)` | 初始化，返回 0 成功 |
| `driver_exit` | `int (*)(dkm_driver_handle *handle)` | 卸载清理，返回 0 成功 |

三个符号均需 `__attribute__((visibility("default")))` 以确保不被隐藏。

### 4.2 driver_desc 字段

```c
const dkm_driver_desc driver_desc = {
    .magic          = DKM_DRIVER_MAGIC,     /* 0x444B4D31 "DKM1"，必须 */
    .abi_version    = DKM_ABI_VERSION,      /* 1 */
    .desc_size      = sizeof(dkm_driver_desc),
    .name           = "hello",
    .version        = "1.0",
    .vendor         = "Deshab",
    .driver_class   = DKM_CLASS_MISC,       /* 类别，见下表 */
    .stage          = 3,                    /* 加载阶段 0-3 */
    .flags          = 0,                    /* DKM_F_* 标志 */
    .priority       = 100,
    .depends        = g_depends,            /* 依赖的 provides 名 */
    .depends_count  = 0,
    .provides       = g_provides,           /* 本驱动提供的能力名 */
    .provides_count = 1,
    .min_kernel_abi = DKM_KERNEL_API_VERSION,
    .feature_bits   = 0,
};
```

**驱动类别**（`driver.h`）：

| 常量 | 值 | 说明 |
|------|----|------|
| `DKM_CLASS_PLATFORM` | 1 | 平台发现 |
| `DKM_CLASS_BUS` | 2 | 总线（PCI） |
| `DKM_CLASS_INTERRUPT` | 3 | 中断控制器 |
| `DKM_CLASS_TIMER` | 4 | 计时器 |
| `DKM_CLASS_CONSOLE` | 5 | 控制台/帧缓冲 |
| `DKM_CLASS_STORAGE` | 6 | 存储（AHCI/NVMe） |
| `DKM_CLASS_FS` | 7 | 文件系统 |
| `DKM_CLASS_NET` | 8 | 网络 |
| `DKM_CLASS_INPUT` | 9 | 输入（键鼠） |
| `DKM_CLASS_GPU` | 10 | GPU |
| `DKM_CLASS_MISC` | 11 | 杂项 |

**驱动标志**（`flags`）：

| 常量 | 说明 |
|------|------|
| `DKM_F_REQUIRED` | 当前 stage 失败则 DRR recovery 或 panic |
| `DKM_F_BOOT_MODULE` | 可由 bootloader 预加载 |
| `DKM_F_NO_UNLOAD` | 不允许卸载 |
| `DKM_F_STRONG_RECOVERY` | data 段使用 UTSM 强恢复策略 |
| `DKM_F_DMA_REQUIRED` | 需要 DMA API |
| `DKM_F_EARLY_LOG` | init 期间允许 early log |

### 4.3 driver_init / driver_exit

```c
int driver_init(const dkm_kernel_api *api, dkm_driver_handle *handle) {
    (void)handle;
    /* 校验 kernel_api 可用性 */
    if (!api || !api->log) return -1;    /* 返回负数=失败 */

    /* 通过 kernel_api 调用内核服务，不直接依赖内核符号 */
    api->log->info("[hello] init");

    /* 注册 IRQ、映射 MMIO、注册 block/net 设备等 */
    return 0;
}

int driver_exit(dkm_driver_handle *handle) {
    (void)handle;
    /* 清理资源 */
    return 0;
}
```

> `required` 驱动 init 失败会触发 DRR recovery 或 panic；非 required 驱动失败仅标记为 `DKM_STATE_FAILED` 并继续。

### 4.4 dkm_kernel_api 使用

驱动通过 `dkm_kernel_api` 调用内核服务，**不直接依赖任意内核符号**。主要能力：

| 字段 | 类型 | 用途 |
|------|------|------|
| `log` | `const dkm_log_api*` | 日志（info/warn/error/panic） |
| `dma` | `const dkm_dma_api*` | DMA 缓冲分配 |
| `net` | `const dkm_net_api*` | 网络设备注册/收发 |
| `block` | `const dkm_block_api*` | block 设备注册/读写 |
| `mmio` | `const dkm_mmio_api*` | MMIO 映射查询 |
| `mm_map_mmio` | `fn` | MMIO 独立窗口映射（PCD\|PWT） |
| `mm_unmap_mmio` | `fn` | 解除 MMIO 窗口映射 |
| `irq_register` | `fn` | 注册 IRQ handler |
| `register_apic_eoi` | `fn` | 注册 LAPIC EOI 钩子 |
| `irq_vector_alloc/free` | `fn` | 动态 IDT 向量分配（MSI/MSI-X） |
| `rsdp_address` | `const void*` | ACPI RSDP 物理地址 |
| `fb_address` / `fb_*` | — | 帧缓冲信息 |
| `hhdm_offset` | `u64` | 物理→虚拟偏移 |
| `boot_modules_response` | `const void*` | Limine boot module 响应 |

### 4.5 manifest 注册

驱动构建为 `.drv` 后，需在 `SYSTEM/driver/manifest.json` 注册：

```json
{
  "name": "hello",
  "path": "driver/misc/hello.drv",
  "class": "misc",
  "required": false,
  "depends": [],
  "provides": ["hello"]
}
```

条目放在对应 stage 的 `drivers` 数组中（stage 0-3）。`required: true` 的驱动 init 失败会触发 DRR recovery 或 panic。

如需 bootloader 预加载（stage0/stage1），还需在 `SYSTEM/boot/limine.conf` 添加 `module_path`。

---

## 5. 头文件索引

### 基础层

| 头文件 | 用途 | 关键 API |
|--------|------|----------|
| `types.h` | 基础类型 | `u8/u16/u32/u64/i8/i16/i32/i64/NULL` |
| `portio.h` | 端口 I/O | `dsb_inb/dsb_outb/dsb_inw/dsb_outw/dsb_inl/dsb_outl` |
| `serial.h` | COM1 串口日志 | `dsb_slog(tag,msg)` / `dsb_swrite(s)` / `dsb_sputc(c)` |
| `tsc.h` | TSC 校准与延时 | `dsb_tsc_calibrate()` / `dsb_rdtsc()` / `dsb_delay_ms/us()` |
| `string.h` | 基础内存操作 | `dsb_memset/dsb_memcpy/dsb_strlen` |

### ABI 层

| 头文件 | 用途 | 关键 API |
|--------|------|----------|
| `kernel_api.h` | kernel_api ABI 结构体 | `dkm_kernel_api` / `dkm_log_api` / `dkm_dma_api` / `dkm_net_api` / `dkm_block_api` / `dkm_mmio_api` |
| `boot_context.h` | 启动上下文 | `dsk_boot_context` / `DSK_BOOT_MAGIC` / `dsk_entry_fn` / `probe_info` / `net_lease_info` |
| `driver.h` | 驱动描述符 | `dkm_driver_desc` / `DKM_DRIVER_MAGIC` / `DKM_CLASS_*` / `DKM_F_*` / `dkm_driver_handle` |
| `block.h` | block 便捷层 | `dsb_block` / `dsb_block_init()` / `dsb_block_read/write()` |

### 应用层

| 头文件 | 用途 | 关键 API |
|--------|------|----------|
| `app.h` | 应用上下文 | `dsb_app_context` / `dsb_app_init()` |
| `fb.h` | 帧缓冲绘制 | `dsb_pixel/fill_rect/fill_bg/fill_rounded_rect/rect_outline/blend` |
| `font.h` | 字体渲染 | `dsb_draw_char/dsb_draw_string`（需先提供 `g_ascii` 位图源） |
| `cursor.h` | 鼠标光标 | `dsb_cursor` / `dsb_cursor_init/save/restore/draw` |
| `input.h` | 键鼠输入 | `dsb_scan_to_ascii()` / `dsb_mouse` / `dsb_mouse_init/poll()` |
| `ui.h` | 色板与装饰 | `DSB_BG_PRIMARY` 等色板 / `dsb_draw_titlebar/statusbar` / `dsb_linuxapp_parse()` |
| `rtc.h` | RTC 时钟 | `dsb_rtc_time(h, m)` |

### 领域层（按需包含，含大 static 缓冲）

| 头文件 | 用途 | BSS 占用 | 关键 API |
|--------|------|----------|----------|
| `fat32.h` | FAT32 读写 + LFN | ~516KB | `f32_init/read_root_file/write_root_file/delete_root_file/list_root/list_dir/read_path_lfn/write_path_lfn` |
| `net.h` | 网络协议栈 | ~64KB | `ns_init/ping/dns_resolve/tcp_connect/tcp_send/tcp_recv/tcp_close` |

### 伞头

| 头文件 | 包含范围 |
|--------|----------|
| `deshab.h` | 基础层 + ABI 层 + 应用层（不含 `fat32.h` / `net.h` / `font.h`） |

> `font.h` 需先提供位图数据源（`#include "ascii_bitmaps.c"`），故不在伞头中默认包含。

---

## 6. ABI 约束

### 6.1 kernel_api 布局不可变

`dkm_kernel_api` 结构体的**字段顺序和类型不可调整**。UTSM 端按固定偏移填充各 API 指针，DKM loader 和 DSK 按相同偏移读取。修改字段顺序会破坏所有已编译的驱动和应用。

### 6.2 字段只追加

新增能力只允许在结构体**末尾追加**字段（尾部追加保持 ABI 兼容）。旧驱动访问不到新字段不受影响；新驱动可通过 `api->size` 判断字段是否存在。

### 6.3 偏移常量

供不便通过结构体解引用的场景（如早期 boot context 解析、汇编代码）使用，消除散落的硬编码偏移：

| 常量 | 值 | 对应字段 |
|------|----|----------|
| `DSB_KAPI_OFF_NET` | `0x48` | `const dkm_net_api *net` |
| `DSB_KAPI_OFF_BLOCK` | `0xA8` | `const dkm_block_api *block` |

常规代码应直接通过 `api->net` / `api->block` 访问，仅在早期启动或指针不可解引用时使用偏移常量。

### 6.4 dsk_boot_context 同理

`dsk_boot_context` 的 `reserved[8]` 槽位有约定索引：

| 索引 | 常量 | 用途 |
|------|------|------|
| 4 | `DSB_BOOT_RESERVED_PE_SERVICE` | PE/EXE 兼容层服务指针 |
| 5 | `DSB_BOOT_RESERVED_LINUX_COMPAT` | Linux 兼容层服务指针 |
| 6 | `DSB_BOOT_RESERVED_PROBE_INFO` | 插桩 probe 缓冲指针 |
| 7 | `DSB_BOOT_RESERVED_NET_LEASE` | netman 网络租约指针 |

---

## 7. 构建参数速查

### 7.1 应用（PIE ELF）

应用编译为 PIE ELF，由 DSK 的 PIE ELF loader 加载（支持 `R_X86_64_RELATIVE` 重定位）。

**clang 编译参数**：

```
-target x86_64-unknown-none
-std=c11
-ffreestanding
-fno-builtin
-fno-stack-protector
-fpie                    # 位置无关可执行文件
-mno-red-zone
-mno-sse -mno-sse2 -mno-mmx -msoft-float   # 未启用 FPU/SSE 前必须
-m64
-Wall -Wextra
-I<sdk/include>          # SDK 头文件路径
-O2
```

**ld.lld 链接参数**：

```
-nostdlib
-static
-pie
-z max-page-size=0x1000
-T linker.ld             # PIE ELF 布局，ENTRY(dsk_entry)
```

**linker.ld 布局**：text（PF_R\|PF_X）→ rodata（PF_R）→ data（PF_R\|PF_W）→ .rela.dyn → .dynamic → .bss（NOBITS）。

### 7.2 驱动（ET_REL）

驱动编译为可重定位对象（ET_REL），由 DKM loader 装载并做 ELF64 relocation（`R_X86_64_64/32/32S/PC32`）。

**clang 编译参数**（与应用的差异）：

```
-fno-pic -fno-pie        # 不可重定位代码（非 PIE）
-mcmodel=kernel          # 内核代码模型
                         # 无 -fpie，无 linker.ld，无链接步骤
```

其余参数（`-target -std -ffreestanding -fno-builtin -mno-red-zone -mno-sse ...`）与应用一致。

**ld.lld 链接参数**：

```
-r                       # 合并为单个可重定位对象（不生成可执行文件）
-o <name>.drv
```

### 7.3 参数差异对照

| 参数 | 应用 | 驱动 |
|------|------|------|
| 位置无关 | `-fpie` | `-fno-pic -fno-pie` |
| 代码模型 | （默认 small） | `-mcmodel=kernel` |
| 链接 | `-nostdlib -static -pie -T linker.ld` | `-r`（仅合并） |
| 产物 | PIE ELF（`.elf`） | ET_REL（`.drv`） |
| 入口 | `dsk_entry` | `driver_desc` / `driver_init` / `driver_exit` |
| 重定位 | `R_X86_64_RELATIVE`（加载时） | `R_X86_64_64/32/32S/PC32`（装载时） |

### 7.4 通用约束

- **进入 C 前必须 `cli/cld`**（由 loader 保证）。
- **未启用 FPU/SSE 前编译必须** `-mno-sse -mno-sse2 -mno-mmx -msoft-float`。
- **DATA/BSS 段使用 `PF_R|PF_W|PF_X`**（SAS-R0 下驱动模块内存需可执行）。
- **无 libc**：所有内存操作使用 `dsb_memset/dsb_memcpy/dsb_strlen`，不可调用标准库函数。
