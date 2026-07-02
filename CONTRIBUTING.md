# 贡献与协作规范

感谢你参与 Deshab-OS 的开发！本文档定义了开发环境、代码规范、Git 工作流与测试验证流程，确保所有贡献遵循统一的工程标准。

> 操作系统开发容错率极低，一行错误的内联汇编或 ABI 字段偏移就可能导致三重故障。请严格遵守以下规范。

---

## 1. 开发环境

### 1.1 必需工具链

| 工具 | 版本要求 | 用途 |
|------|---------|------|
| **LLVM Clang** | 14+ | C11 / 汇编编译（`-target x86_64-unknown-none`） |
| **ld.lld** | 14+ | 链接器（内核 ELF + 驱动 `.drv`） |
| **make** | 任意 | 各模块 MAKEFILE |
| **Python 3** | 3.8+ | 字体生成（`mkfont.py`） |
| **Pillow** | 最新 | Python 图像库（`pip install Pillow`） |
| **QEMU** | 7+ | `qemu-system-x86_64`，UEFI 启动验证 |
| **PowerShell** | 5.1+ | `build.ps1` 构建 + GPT/FAT32 镜像打包 |

> `build.ps1` 会自动扫描 `C:\Program Files\LLVM\bin` 等常见目录发现 clang/ld.lld，即使不在 PATH 也可用。

### 1.2 可选工具

- **Limine**：引导器（`SYSTEM/EFI/BOOT/BOOTX64.EFI` 已内置）
- **xxd / hexdump**：二进制镜像校验（MBR `AA55`、GPT `EFI PART`、FAT32 标记）

### 1.3 环境验证

```powershell
# 完整构建
.\build.bat

# 应产出：
#   SYSTEM/boot/utsm.elf
#   SYSTEM/system/deshab64/deshab.elf
#   SYSTEM/driver/**/*.drv（14 个）
#   ISO/deshab.img（GPT + FAT32 ESP）

# QEMU 启动验证
.\ISO\run_qemu.bat
# 串口输出应看到：
#   [UTSM] boot → ... → [UTSM] SELFTEST PASS → [UTSM] loading DSK → [DSK] boot → [DSK] SELFTEST PASS
```

---

## 2. 目录结构约定

```text
CODE/               源码（只放源码，不放产物）
├── UTSM/           首阶段内核（→ SYSTEM/boot/utsm.elf）
├── dsk/            DSK 主内核（→ SYSTEM/system/deshab64/deshab.elf）
├── DKM/            驱动源码（每个驱动一个子目录 → SYSTEM/driver/<class>/<name>.drv）
├── firstInit/      用户设置向导（→ SYSTEM/system/user/use/FirstInit.elf）
├── mouse/          鼠标初始化器（→ SYSTEM/system/deshab64/mouse/mouseInit.elf）
├── netman/         网络管理器（→ SYSTEM/system/deshab64/network/netman.elf）
└── font/           字体工具（→ SYSTEM/system/font/*.dbf）

SYSTEM/             打包产物（IMG 根目录，提交到仓库供构建使用）
├── boot/           引导模块
├── driver/         *.drv + manifest.json
├── system/         主内核 + 字体 + 用户程序
└── EFI/BOOT/       UEFI 引导

RE/                 研究与设计文档（架构/算法/ABI 规范）
docs/               项目文档（架构总览/路线图）
```

**约定**：

- `CODE/` 只放源码，`SYSTEM/` 放构建产物。修改源码后必须重新构建并更新 `SYSTEM/` 下的产物。
- `*.o` 中间文件不应提交（各 MAKEFILE 产物）。`.gitignore` 已忽略 `ISO/deshab.img`。
- 新增驱动必须在 `SYSTEM/driver/manifest.json` 注册，并在 `limine.conf` 添加 `module_path`。
- 新增子系统设计文档放 `RE/`，项目级文档放 `docs/`。

---

## 3. 代码规范

### 3.1 C 代码

**编译标志**（内核与驱动统一，源自 `build.ps1`）：

```text
-target x86_64-unknown-none -std=c11 -ffreestanding -fno-builtin
-fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64
-Wall -Wextra -O2
```

**强制规则**：

1. **进入 C 前必须 `cli` 和 `cld`**——未建 IDT 前被中断或方向标志异常会导致三重故障。
2. **未启用 FPU/SSE 前禁止生成 SIMD 指令**——Clang 在 freestanding 下也可能生成 `xorps/movups`，必须用 `-mno-sse -mno-sse2 -mno-mmx -msoft-float`。
3. **高半区内核必须用 `-mcmodel=kernel`**——否则 `R_X86_64_32 out of range`。
4. **`driver_desc` 必须是 global**——`__attribute__((visibility("default")))`，否则 symtab 中为 LOCAL，loader 找不到。
5. **不要用 `__attribute__((unused))` 标记需要被调用的函数**——在 `-O2` 下可能被优化掉，导致运行时崩溃。
6. **ABI 结构体字段只能追加，不能插入**——`dkm_kernel_api` 新增字段必须追加到结构尾部，否则改变已有驱动本地 ABI 偏移。
7. **日志必须带稳定模块前缀**——如 `[UTSM]`、`[DKM:ahci]`、`[DSK]`，便于 QEMU 日志过滤。

**代码风格**：

- 4 空格缩进，`{` 同行（K&R 风格）。
- 函数/变量命名 `snake_case`，宏/常量 `UPPER_SNAKE_CASE`。
- 结构体用 `typedef struct { ... } name_t;`，类型后缀 `_t`。
- 内联汇编约束严格验证（如 `rdmsr =A` 在真机会失败，QEMU 侥幸能工作）。

### 3.2 汇编

- 汇编入口文件命名避免与 C 同名（`idt.S` 和 `idt.c` 同名会输出冲突的 `idt.o`，应拆为 `idt_stubs.S`）。
- 中断 stub 宏必须显式区分是否自动压入 error code，否则破坏统一栈帧导致 `iretq` 失败。
- Limine request 必须放入 `.limine_requests*` 区段，linker script 显式保留 start/request/end。

### 3.3 驱动开发

新增 DKM 驱动必须：

1. 在 `CODE/DKM/<class>/<name>/` 创建源码，导出 `driver_desc` / `driver_init` / `driver_exit`。
2. 驱动本地 `dkm_kernel_api` 定义必须与内核侧**字段布局完全一致**（含 padding）。
3. 通过 `dkm_kernel_api` 调用内核服务，**不直接依赖任意内核符号**。
4. 第一版只支持必要 relocation，禁止 TLS / IFUNC / lazy binding / 外部动态库 / 用户态 libc。
5. 在 `SYSTEM/driver/manifest.json` 注册（name / path / class / required / depends / provides）。
6. 在 `SYSTEM/boot/limine.conf` 添加 `module_path` 预加载。
7. 在 `build.ps1` 的 `Build-DkmDriver` 流程中添加编译目标。
8. 更新 [CODE/DKM/README.md](CODE/DKM/README.md) 驱动清单。

### 3.4 构建脚本

- 不要用 PowerShell `Set-Content` 写 C 源文件——会产生 BOM（`U+FEFF`），导致 clang 报错 `extraneous closing brace`。
- 构建脚本修改编译参数后必须先 `make clean`，否则旧 object 仍按旧参数链接。
- 打包 SYSTEM 时过滤 `*.tmp*`，避免旧 `utsm.elf.tmp` 写入镜像。
- `ISO/deshab.img` 被 QEMU 占用时打包会失败，重新打包前先关闭 QEMU。

---

## 4. Git 工作流

### 4.1 分支模型

| 分支 | 用途 |
|------|------|
| `main` | 稳定主线，始终可构建可启动 |
| `feat/<name>` | 功能开发分支 |
| `fix/<name>` | 修复分支 |
| `docs/<name>` | 文档分支 |

### 4.2 提交规范

采用 [Conventional Commits](https://www.conventionalcommits.org/) 格式：

```text
<type>(<scope>): <subject>

<body>
```

**type**：

| type | 说明 |
|------|------|
| `feat` | 新功能（新驱动、新子系统） |
| `fix` | 修复 |
| `docs` | 文档变更 |
| `build` | 构建系统变更 |
| `refactor` | 重构（不改行为） |
| `test` | 测试 |
| `chore` | 杂项 |

**scope** 示例：`utsm` / `dsk` / `dkm` / `ahci` / `fat32` / `firstInit` / `build`

**示例**：

```text
feat(dkm/nvme): 实现 NVMe admin queue 与 identify

- 新增 admin submission/completion queue 分配
- 实现 identify controller 命令
- 注册 nvme0 block provider

依赖 Phase 0 高位 PCI MMIO 映射。
```

```text
fix(utsm): 修复 rdmsr 内联汇编在真机三重故障

rdmsr =A 约束在 QEMU 侥幸工作但真机失败，
改为显式 eax/edx 输出。
```

### 4.3 提交粒度

- 一个提交只做一件事。
- 内核/驱动变更必须附带构建产物更新（`SYSTEM/` 下的 `.elf` / `.drv`）。
- 修改设计文档（`RE/`）应在同一提交更新对应模块 README。
- 阶段性成果应在 [LESSONS_LEARNED.md](LESSONS_LEARNED.md) 追加经验记录。

---

## 5. Pull Request 流程

### 5.1 提交前检查清单

- [ ] `.\build.bat` 构建成功，产出 `ISO/deshab.img`
- [ ] `.\ISO\run_qemu.bat` 启动到 `[DSK] SELFTEST PASS`
- [ ] 无新增编译 warning（`-Wall -Wextra`）
- [ ] ABI 变更已同步所有驱动本地定义
- [ ] 新驱动已在 `manifest.json` + `limine.conf` 注册
- [ ] 设计文档与 README 已同步
- [ ] 提交信息符合 Conventional Commits
- [ ] [LESSONS_LEARNED.md](LESSONS_LEARNED.md) 已追加经验记录（如有）

### 5.2 PR 描述模板

见 [.github/pull_request_template.md](.github/pull_request_template.md)。

### 5.3 审查重点

审查者应重点关注：

1. **ABI 兼容性**——是否在结构体中间插入字段？
2. **早期启动安全**——是否在 IDT 未建前开中断？是否生成 SIMD 指令？
3. **DMA 地址正确性**——是否用内核虚拟地址反推物理地址喂给设备？
4. **热路径约束**——是否在调度路径做重活（扫描 capability / 计算 MAC / 重加密）？
5. **恢复路径独立性**——是否让 DRR 依赖普通堆？
6. **日志前缀**——是否带稳定模块前缀？

---

## 6. QEMU 测试验证

### 6.1 默认启动验证

```powershell
.\ISO\run_qemu.bat
```

**预期串口输出**（最小验证标准）：

```text
[UTSM] boot
[UTSM] SELFTEST PASS
[UTSM] loading DSK
[DSK] boot
[DSK] context ok
[DSK] SELFTEST PASS
```

### 6.2 设备测试场景

不同驱动需要显式挂载测试设备（默认 QEMU 脚本不含）：

| 驱动 | QEMU 参数 | 验证 |
|------|----------|------|
| ahci | 临时 SATA 测试盘 | `ahci0` block provider 注册，FAT32 读取 |
| nvme | `-device nvme` | 探测 `1b36:0010`（register 读取依赖高位 MMIO） |
| e1000 | `-device e1000e` | 探测 `8086:10d3`，RX/TX ring `tx=ready rx=ready` |
| virtio_net | `-netdev user` + `-device virtio-net-pci` | 探测 `1af4:1001` |

> 加入多个网卡会改变 PCI 拓扑，驱动必须把"设备未找到"视为 optional 正常路径。

### 6.3 真机测试注意

QEMU 不严格检查 NX，真机 Limine page table 会强制 NX。以下问题只在真机暴露：

- `rdmsr =A` 内联汇编 bug
- 早期未设置 `IA32_GS_BASE` 导致栈保护 prologue 页错误
- arena 分配的内存在真机可能不可执行

真机调试时，`kernel_panic` 应直接通过 COM1 串口输出，不依赖 console/heap。

---

## 7. 文档维护规范

### 7.1 文档分层

| 层级 | 位置 | 用途 |
|------|------|------|
| 设计规范 | `RE/` | 完整架构/算法/ABI 规范 |
| 模块说明 | `CODE/<module>/README(.md)` | 模块落地说明与实现清单 |
| 项目文档 | `docs/` | 架构总览、路线图 |
| 项目状态 | `CLAUDE.md` | 当前状态与下一阶段路线 |
| 经验记录 | `LESSONS_LEARNED.md` | 每次开发的经验与教训 |

### 7.2 同步规则

- 完成阶段性目标后**必须同步** `CLAUDE.md` 路线，避免残留旧状态误导后续开发。
- 修改 `RE/` 设计后应同步对应模块 README。
- 新增/修改驱动后更新 [CODE/DKM/README.md](CODE/DKM/README.md) 驱动清单。
- 架构变更应更新 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

### 7.3 经验记录格式

每项重要开发应在 [LESSONS_LEARNED.md](LESSONS_LEARNED.md) 追加：

```markdown
## YYYY-MM-DD — <简述>

### 经验
1. ...
2. ...

### 教训
1. ...
2. ...

### 当前已完成 / 下一步计划（可选）
```

---

## 8. 相关文档

- [架构设计文档](docs/ARCHITECTURE.md)
- [开发路线图](docs/ROADMAP.md)
- [CLAUDE.md](CLAUDE.md) — 项目当前状态
- [CODE/UTSM/README.md](CODE/UTSM/README.md) — UTSM 模块设计
- [CODE/DKM/README.md](CODE/DKM/README.md) — DKM 驱动系统
- [LESSONS_LEARNED.md](LESSONS_LEARNED.md) — 开发经验教训
