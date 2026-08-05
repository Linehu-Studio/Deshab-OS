# Deshab-OS 应用 Linux 预览功能

## 概述

允许在 Linux 开发机上预览 Deshab-OS 应用的 UI 效果。同一份 UI 绘制代码可以在 Linux 上通过 SDL2 后端预览，在 Deshab-OS 上通过 framebuffer 后端运行。

## 架构设计

```
┌────────────────────────────────────────────────────                          ─┐
│                  应用源代码 │
│  (使用 da_draw_char, da_fill_rect 等绘制函数)        │
└──────────────────┬──────────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        │  #ifdef DA_LINUX_PREVIEW  │
        └──┬──────────────┬────────────┘
           │              │
            Linux 后端 Deshab-OS 后端
               
              (SDL2) (framebuffer) │              │
     X11/Wayland 窗口物理显示器
```

## 文件结构

```
CODE/tools/notes/
├── main.c              # Deshab-OS 原生应用入口
├──  Makefile            # Deshab-OS 编译用Makefile
├── notes_preview.c     # Linux 预览版（包含完整 dsk_entry 实现）
├── Makefile.linux      # Linux 编译用 Makefile
├── desktop_app_linux.h # Linux SDL2 后端
└── README_LINUX_PREVIEW.md  # 本说明文档
```

## 使用方法

### 1. 在 Linux 上编译预览版

```bash
cd CODE/tools/notes
make -f Makefile.linux
```

### 2. 运行预览版

```bash
./notes_preview
```

### 3. 退出预览

按 `ESC` 键关闭窗口。

### 4. 清理

```bash
make -f Makefile.linux clean
```

## 开发工作流

```

  1. 在 Linux 上编辑代码 ↓
2. make -f Makefile.linux 编译预览版
   ↓
3. ./notes_preview 查看 UI 效果
   ↓
4. 调试布局和渲染
   ↓
5. 测试通过后，用原生的 Makefile 编译到 Deshab-OS
   ↓
6. 在 Deshab-OS 中运行验证
```

## 支持的 UI 元素

| 元素 | Linux 预览 | Deshab-OS |
|------|-----------|-----------|
| 背景填充 | ✅ | ✅ |
| 矩形填充 | ✅ | ✅ |
| 圆角矩形 | ✅ | ✅ |
| ASCII 字符 | ✅ | ✅ |
| 字符串 | ✅ | ✅ |
| 标题栏 | ✅ | ✅ |
| 状态栏 | ✅ | ✅ |
| 鼠标 光标 | ✅ | ✅ |
|键盘输入 | ✅ | ✅ |
| 鼠标移动 | ✅ | ✅ |

## 扩展到其他应用

要为其他应用（如 fileman, editor, proedit）添加 Linux 预览支持：

1. 复制 `notes_preview.c` 作为模板
2. 修改 `main()` 中的 UI 逻辑
3. 使用相同的 `desktop_app_linux.h

` 后端

## 依赖- SDL2 (`libsdl2-dev` on Debian/Ubuntu)
- GCC 或 Clang
- Make

## 注意事项

 1. **键盘映射**：SDL2 的键盘扫描码需要映射到 Deshab的 PS/2 扫描码
2. **鼠标行为**：SDL2 鼠标事件转换为 Deshab 的鼠标数据包格式
3. **字体数据**：需要从 `ascii_bitmaps.c` 获取字模数据
4. **性能**：Linux 预览版主要用于 UI 调试，不是完整的功能测试