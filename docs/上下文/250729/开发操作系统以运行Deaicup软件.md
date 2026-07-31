# 开发操作系统以运行Deaicup软件

## 软件需求分析

**Deaicup Software** 是一个 Rust/egui GUI 应用：
- 侧边栏导航 + 页面切换
- 依赖：Rust std 运行时、egui 渲染后端（OpenGL/Vulkan）、窗口系统、网络（HTTPS 加载图片）、文件系统

## 操作系统现状

- **原生环境**：Ring0 单地址空间，自有 framebuffer UI 框架（deshab_ui.h），无 Rust 运行时、无 GPU 驱动、无窗口服务器
- **Linux 兼容层**：有 Arch rootfs + IPC exec/file，但**无显示服务器、无 GPU、无输入转发**

## 运行路径

运行 Rust/egui 应用有两条路径：
1. **Linux guest 路径**：在 Linux guest 内运行，需要 X11/Wayland 显示服务器 + GPU/Virtio-GPU + 输入转发
2. **原生路径**：将 egui 渲染后端适配到 Deshab framebuffer，需要 Rust std 移植 + 自定义 eframe 后端

两条路径都需要大量基础设施工作，无近路。
