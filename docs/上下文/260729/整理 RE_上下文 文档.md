# 整理 RE/上下文 文档

## 文档地图（共 18 份 + 8 份测试用例）

| 层级 | 文档 | 内容 |
|------|------|------|
| 内核设计层 (`docs/RE/`) | UTSM_设计架构.md | UTSM 架构定稿：模块组成、内存模型、数据结构、UTRW、DRR 协作 |
| | UTSM_算法记录.md | UTSM v1 核心算法：段创建、密钥派生、Fast/Slow Read/Write、PCKC、Checkpoint、Rollback |
| | 驱动模块ABI设计.md | DKM ABI：driver_desc/driver_init/driver_exit、kernel_api、ELF relocation、manifest、状态机、boot module |
| | Bug记录.txt | 8 条未结构化 bug 记录 |
| 系统架构层 (`docs/`) | ARCHITECTURE.md | 系统整体架构：SAS-R0-PCQ/UTSM/UTRW/DRR/DKM 五大核心支柱 |
| | BOOT_SEQUENCE.md | 五阶段启动：Ignis/Sigillum/Transitus/Origo/Excitas |
| | ROADMAP.md | Phase 0-9 分阶段开发路线图 |
| 开发记录层 (`docs/上下文/`) | 6 份对话记录 | 双内核/Linux EXE/桌面 UI/Pacman 等实现细节 |
| 测试用例 (`docs/RE/test-cases/`) | 8 份 | boot-regression/driver-matrix/linux-compat/network-e2e/pe-compat/performance-baseline/userapps-matrix/x86emu32-coverage |

## 关键同步差距（文档 vs 实现态）
- ROADMAP 滞后：网络协议栈已实现却标"无协议栈"；未收录桌面/Linux/PE 兼容层/Pacman
- ARCHITECTURE 仍描述"旋转加载环"，实际已被静态 Logo 取代
- BOOT_SEQUENCE 标 B4 正常路径"仅 halt"，实际 login → desktop 已实现

---

## 兼容层子系统技术摘要

### 双内核架构（UTSM + Linux）

**核心设计**：VMX/EPT 完整虚拟化，UTSM 作为 Type-1 hypervisor（monitor 模型），Linux 6.6 LTS guest 运行在 VMX non-root 模式。

**GPA 布局**（Phase 1.3 修复后）：

| GPA | 用途 | 大小 |
|-----|------|------|
| `0x00400000 - 0x02FFFFFF` | initrd / boot_params | - |
| `0x03000000 - 0x03FFFFFF` | Linux guest initrd | 16MB+ |
| `0x04000000` | IPC 共享内存 | 1MB |
| `0x05000000+` | Linux guest RAM | - |

**e820 约定**：`linux_loader.c` 中 IPC 区域为 `reserved` 类型，RAM 移至 0x05000000。

**Hypercall ABI**：VMCALL，guest RAX=操作码，RDI/RSI/RDX=参数，返回值写回 RAX。9 种 op：PING/HELLO/SHM_INFO/CAP_VALIDATE/UTRW_READ/UTRW_WRITE/DRR_CHECKPOINT/CONSOLE_WRITE/CONSOLE_READ。

**IPC EXEC 协议**（消息号 16-20）：EXEC_REQUEST/STDOUT/STDERR/EXIT/READY。`ipc_exec_request` ≤240 字节。

**virtio-mmio**：blk→IRQ5(0x35)，net→IRQ6(0x36)，MMIO 处理通过 EPT misconfig exit。

### PE/EXE 兼容层

四个组件（`CODE/UTSM/pe/`）：
1. **pe_service.c** — 统一服务入口，通过 `reserved[4]` 暴露
2. **pe_loader.c** — PE32/PE32+ 解析映射重定位
3. **pe_shim.c** — IAT 拦截，`__attribute__((ms_abi))` shim，`__builtin_setjmp/longjmp` 异常恢复
4. **x86emu32.c** — x86-32 指令解释器

### shell `linux` 命令实现

`CODE/shell/main.c`：`cmd_linux` → `g_lxc_svc->exec()` → IPC → linux_resume → daemon fork+exec → pipe → IPC 回传 → 显示

---

## 桌面 UI 技术摘要（Winux-Kate "Sealed Arc" 设计语言）

### 架构
- `desktop.elf` 入口 `dsk_entry(dsk_boot_context)`，三层：顶栏(34px) + 页面宿主 + 任务栏
- 三页面：DASH/IDE/DESK（IM 页移除，自定义页 ID 从 4 起）
- DASHBOARD 四面板：TERM-01/02(35%) + EDITOR/FILES(65%)

### 配色方案（`deshab_ui.h:744-761`）

| 语义 | 常量 | 值 |
|------|------|-----|
| 主背景 | `KS_BG_PRIMARY` | `0xFF02040A` |
| 次级背景 | `KS_BG_SECONDARY` | `0xFF0A1428` |
| 主文本 | `KS_TEXT_PRIMARY` | `0xFFE8F4FC` |
| 暗文本 | `KS_TEXT_DIM` | `0xFF90B0C8` |
| 霓虹青 | `KS_ACCENT` | `0xFF00E5FF` |
| 霓虹绿 | `KS_ACCENT2` | `0xFF39FF14` |
| 危险红 | `KS_DANGER` | `0xFFFF4D6D` |
| 边框 | `KS_BORDER` | `0xFF2A6070` |

对比度：TEXT_DIM 在 BG_PRIMARY 上 10.8:1，BORDER 8.2:1。

### 渲染优化
- 双缓冲：`g_fb.fb` → `SPRITE_BUF_ADDR=0x7000000` 离屏，`flip_buffer()` 全屏 / `flip_rect()` 局部
- DBF 位图字体：`simhei_16/24/32.dbf`，GB2312 全部 6763 汉字 + ASCII，二分查找
- 视觉效果简化：5 层→3 层，扫描线改空函数，圆角改 0
- desktop.elf 体积 802KB→140KB（BSS 分离为 NOBITS）

### FILES 面板过滤
跳过 `.ELF` 可执行和 `FIRSTINI` 前缀，保留用户文件（USER.CON/NETCONF.CNF/FIRSTINT.XT）。

---

## Pacman 包管理方案

**核心矛盾**：pacman 依赖 glibc+POSIX+动态链接+网络+加密+压缩+可写 Unix FS，Deshab 均不提供。

**方案 C（IPC 委托执行）**：`SYSTEM/bin/` 放 stub → PATH 命中 → IPC 委托 Linux guest 执行 → 结果显示回 Deshab。

---

## BOOT_SEQUENCE 技术摘要

### 五阶段启动模型

| 阶段 | 代号 | 产物 | 锚点 |
|------|------|------|------|
| B0 | Ignis | utsm.elf 入内存 | — |
| B1 | Sigillum | 14 DKM 驱动 ACTIVE | `[UTSM] SELFTEST PASS` |
| B2 | Transitus | dsk_boot_context 就绪 | `[DSK] context ok` |
| B3 | Origo | firstInit 标志判定 | `[DSK] first init detected` 或 `SELFTEST PASS` |
| B4 | Excitas | 用户可交互 | `[DSK] jumping to FirstInit` |

### dsk_boot_context

```c
struct dsk_boot_context {
    u64 magic;          // 0x44534B31424F4F54 ("DSK1BOOT")
    u32 abi_version;    // 1
    u32 size;
    u64 flags;          // FROM_UTSM | DKM_READY | FAT32_PATH
    u64 hhdm_offset, rsdp_address;
    u64 framebuffer_address/width/height/pitch/bpp;
    u64 boot_modules_response;
    u64 dkm_kernel_api, dkm_driver_table, driver_count;
    u64 utsm_state, drr_state;
    u64 memory_map, count, entry_size;
    u64 kernel_stack_top;
    u64 reserved[8];    // reserved[4]=pe_svc, reserved[5]=linux_compat_svc
};
```

ABI 约束：字段只能追加到 reserved 区。block_read 通过硬偏移 `api + 0xA8` 读取。

### 失败恢复链
- B2 FAT32 不可用 → 回退 Limine module
- B3 无 block device → 跳过 firstInit
- B1 required 驱动失败 → DRR recovery / panic
- B1 optional 驱动失败 → 标记 FAILED 继续

---

## 整体脉络

```
BOOT_SEQUENCE (B0→B4) → Winux-Kate 桌面 UI → Pacman 包管理
   启动契约                用户交互层           软件生态
```

- UTSM 是契约源（dsk_boot_context 版本化传递）
- DKM 是能力供给（14 驱动暴露 kernel_api）
- DSK 是分发中枢（首次启动调度 + ELF 加载器）
- DRR 是恢复后盾（当前 stub，未来三级回退）
- SAS-R0 是约束源头（所有应用为静态 PIE ELF，无 POSIX/syscall/fork/exec）
