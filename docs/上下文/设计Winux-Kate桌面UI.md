# 设计Winux-Kate桌面UI

## 需求

Deshab 桌面完全复刻 Winux-Kate（`D:\Code\Winux-Kate`）结构，不是仿照。

---

## Winux-Kate 技术栈

- 前端: React + TypeScript + Vite
- 后端: Rust + Tauri
- 核心组件: `window_manager.rs`, `apps.rs`, `DesktopPage.tsx`, `TaskView.tsx`
- 样式: `theme.css`, CSS Modules

---

## Kate 桌面结构

### 三层布局

```
┌──────────────────────────────────────────┐
│ Topbar (34px)                            │
│ 品牌 | WS-1|0 工作区 | DASH/IDE/DESK | PG│
├──────────────────────────────────────────┤
│ Page Host                                │
│ (DASHBOARD / IDE / DESKTOP / Custom)     │
├──────────────────────────────────────────┤
│ Statusbar / Taskbar (24px)               │
└──────────────────────────────────────────┘
```

### 页面结构

| 页面 | Kate 组件 | Deshab 实现 |
|------|-----------|-------------|
| **1.DASHBOARD** | 四面板布局 | TERM-01/02 + EDITOR + FILES |
| **2.IDE** | `.ide-tabs` + dot + embed-empty | dot(39FF14) + +NEW IDE 按钮 + credit |
| **3.DESKTOP** | DesktopGrid 图标 + 任务栏 | 左上网格(104px列宽,18px gap,24px pad) + 窗口 + 任务栏 |

### 顶栏元素

- 品牌: DESHAB DEAICUP STUDIO
- 工作区切换: WS-1\|0, WS-2\|1 等
- 页面切换按钮: DASH / IDE / DESK
- PG 时钟

### 任务栏（DESKTOP 页）

- 窗口项（最大120px宽度）
- ADOPT（收纳 = 所有窗口收归当前工作区并取消最小化）
- 署名 DEAICUP
- EXIT 按钮（KS_DANGER 0xFFFF4D6D 红色）

---

## Kate 色值体系（deshab_ui.h）

```c
/* 背景层级 */
#define KS_BG_PRIMARY    0xFF02040Au  // 深空黑
#define KS_BG_SECONDARY  0xFF0A1428u  // 次级背景
#define KS_BG_TERTIARY   0xFF101C30u  // 三级背景

/* 文本色 */
#define KS_TEXT_PRIMARY  0xFFE8F4FCu  // 接近纯白
#define KS_TEXT_DIM      0xFF90B0C8u  // 暗文本（对比度10.8:1）
#define KS_TEXT_INVERT   0xFF02040Au  // 反色

/* 强调色 */
#define KS_ACCENT        0xFF00E5FFu  // 霓虹青（主强调）
#define KS_ACCENT2       0xFF39FF14u  // 霓虹绿（dot标识）
#define KS_ACCENT_DIM    0xFF2A4A5Au  // 暗青（hover）

/* 边框色 */
#define KS_BORDER        0xFF2A6070u  // 对比度8.2:1
#define KS_BORDER_FOCUS  0xFF00E5FFu
#define KS_BORDER_DIM    0xFF1A2A40u

/* 状态色 */
#define KS_DANGER        0xFFFF4D6Du  // EXIT按钮
#define KS_WARN          0xFFFFCC00u
```

### 优化后的视觉效果

- 面板: 仅保留背景+边框+标题栏（移除光晕+扫描线）
- `du_kate_glow_border()` 简化为单层边框
- 窗口标题栏: 纯色替代渐变
- hover 状态: 同时改变背景和边框
- active 状态: 强调色背景 + 反转文字

---

## Boot 打字机动画

- 标题每字符: busy_delay(2000000)
- 副标题停顿: busy_delay(2500000)
- 打字机每字符: busy_delay(125000)
- 最后停留: busy_delay(10000000)

---

## 双缓冲消除闪烁

```c
u32 *g_real_fb;              // 真实 framebuffer
// g_fb.fb 指向 SPRITE_BUF_ADDR (0x7000000) sprite buffer

void flip_buffer(void);      // 全屏 redraw 后一次性复制
void flip_rect(x, y, w, h); // 仅光标移动时局部复制（24x24）
```

主循环两条路径：
- `need_redraw` → 全屏 redraw + flip_buffer()
- `moved` → 局部 flip_rect()

---

## 性能优化

### 1. 亮度调整延迟应用

```c
static int g_bright_applied = 100;
// g_bright >= 100 时跳过；g_bright == g_bright_applied 时跳过
// 仅在亮度变化时处理全屏（从60Hz降到1-2次/分钟）
```

### 2. 终端增量渲染

```c
typedef struct {
    u8 dirty_rows[TERM_MAX_ROWS];  // 脏行标记
    int any_dirty;                  // 全局脏标记
} bash_state;
// any_dirty=0 时跳过绘制；只绘制 dirty_rows[r]=1 的行
```

### 3. 脏矩形渲染

```c
#define MAX_DIRTY_RECTS 16
typedef struct { int x, y, w, h, active; } dirty_rect;
// merge_dirty_rects() 合并为包围盒
// 光标移动: 只 flip 24x24 区域
// 窗口拖拽: flip 窗口区域
```

### 4. 简化视觉效果

- 禁用扫描线: `du_kate_scanlines()` 改为空函数
- 消除窗口圆角: `DU_RADIUS_MD/LG` 从 8/12 改为 0
- 渐变缓存: 避免重复计算

### 5. 加速内存复制

```c
// flip_buffer() 使用 rep movsq 批量复制
__asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(total_u64) : : "memory");
```

---

## 关键实现细节

### FILES 面板

- 过滤 ELF 可执行文件和 FIRSTINI 前缀
- 保留 user 文件: USER.CON, NETCONF.CNF, FIRSTINT.XT
- 标题: `FAT32:/USER`

### 桌面图标网格

```c
#define KATE_ICON_GAP   16     // 图标间隙（原18，优化后16）
// 左上网格: 104px 列宽, 从左→右排列换行
// 图标边框: KS_ACCENT(00E5FF) 青色
```

### 右键菜单

- "移到工作区"菜单**排除当前工作区**
- 无其他工作区时显示空态

### 自定义页（第 4+ 页）

- ide-tabs 栏: dot + 页名 + +NEW INSTANCE 按钮 + hint + credit
- 自定义页 id 从 4 开始（内置3页: DASHBOARD/IDE/DESKTOP）

### DASHBOARD 面板比例

- 左侧终端: 35%（原42%，优化后缩窄）
- 右侧 FILES/EDITOR: 65%（原58%，优化后加宽）

---

## 验证结果

- desktop.elf 编译零警告，140312 bytes（BSS 正确分离）
- QEMU 验证: 四页面 Ctrl+Tab 切换正确
- 页面按钮: DASH/IDE/DESK（精简后）
- 双缓冲: 鼠标移动和滑块拖动无闪烁
