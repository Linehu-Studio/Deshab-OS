# 经验记录

本文件记录项目开发和架构设计过程中的经验与教训。

## 2026-06-29 — 记录 UTSM 架构与算法设计

### 经验

1. **架构分层**: 将 UTSM 拆成 UTSM、UTRW、DMP、PCKC，并和 SAS-R0-PCQ、DRR 明确边界，能避免调度、加密、恢复逻辑互相污染。
2. **热路径约束**: 将段定位设计为 `segment_slot + generation + epoch` 的 O(1) 路径，能保留 Per-CPU O(1) 调度器的性能目标。
3. **文档同步**: 同时维护模块 README、算法记录和架构记录，方便后续实现时按模块拆分代码。

### 教训

1. **避免破坏连续内存**: UUID 不应嵌入数据区形成孔洞，否则会破坏 cache line、DMA、checkpoint 和批量复制路径。
2. **恢复根要独立**: 看门狗如果只是普通内核线程，无法在调度器损坏或关中断卡死时可靠恢复，必须作为 DRR 独立设计。
3. **不要在调度路径做重活**: 调度切换中不能扫描 capability、计算 MAC 或重加密，否则会破坏 O(1) 调度目标。

## 2026-06-29 — 优化 UTSM 恢复与 checkpoint 设计

### 经验

1. **先补恢复底座**: DRR Emergency Pool、A/B checkpoint metadata、Dirty Shard 是 UTSM 进入实现前最关键的三项稳定性优化。
2. **分片降低抖动**: Dirty Shard 让 checkpoint 从扫描大 bitmap 变为处理 dirty shard，符合“不扫描全内存”的约束。
3. **双槽保证稳定点**: A/B metadata 通过先写 inactive slot、CRC 校验、最后原子切换 active slot，能避免半写 checkpoint 破坏旧稳定点。

### 教训

1. **恢复路径不能依赖普通堆**: 普通 allocator OOM 时，如果 DRR 也依赖它，系统会失去最后恢复能力。
2. **checkpoint 提交顺序必须固定**: metadata、dirty index、MAC root、CRC、flush、active slot 切换的顺序不能随意调整。
3. **Dirty 计数要避免共享争用**: Dirty Shard 的 dirty_count 应结合 per-CPU dirty list，避免多核写入时形成 cache line 热点。

## 2026-06-29 — 添加 SYSTEM 驱动 manifest

### 经验

1. **根目录语义明确**: SYSTEM 打包成 IMG 后就是系统根目录，启动路径和驱动路径应统一以 SYSTEM 为根来描述。
2. **JSON 清单更适合扩展**: manifest.json 能直接表达 stage、required、depends、provides、driver class 等加载信息。
3. **分阶段加载清晰**: platform、boot、filesystem、optional 四阶段能区分启动关键驱动和普通可选驱动。

### 教训

1. **不要只依赖目录扫描**: 启动驱动顺序需要 manifest 控制，否则依赖和失败策略不稳定。
2. **required 标志必须明确**: 启动关键驱动失败应进入 DRR/panic，可选驱动失败只标记 FAILED。
3. **stage0/stage1 需要 boot module 兜底**: 文件系统未就绪前，早期驱动不能完全依赖 SYSTEM/driver 的普通遍历。

## 2026-06-29 — 设计 DKM 驱动模块 ABI

### 经验

1. **ABI 固定入口**: 每个驱动统一导出 `driver_desc`、`driver_init`、`driver_exit`，能让 DSM loader 简化加载和错误处理。
2. **kernel_api 降低耦合**: 驱动通过 `dkm_kernel_api` 调用 log、mem、UTSM、IRQ、PCI、DMA、VFS、NET、DRR 等服务，避免直接绑定任意内核符号。
3. **早期模块双路径**: stage0/stage1 优先从 boot module table 加载，VFS 可用后再从 SYSTEM 路径加载，能覆盖文件系统未启动阶段。

### 教训

1. **ELF 支持范围要收窄**: 第一版只支持必要 x86_64 relocation，禁止 TLS、IFUNC、lazy binding 和外部动态库，避免 loader 复杂度失控。
2. **状态机要显式**: DISCOVERED 到 ACTIVE 以及各类 FAILED 状态必须明确，DRR 才能做恢复和归因。
3. **manifest 和 driver_desc 都要校验**: manifest 描述启动策略，driver_desc 描述模块自声明，二者不一致时应拒绝或降级加载。

## 2026-06-29 — 编写 CODE/DKM/README.md

### 经验

1. **模块 README 要可实现**: DKM README 不只描述概念，还应列出 loader、parser、relocation、state table 等第一版实现清单。
2. **运行路径要闭环**: 从 `SYSTEM/driver/manifest.json` 到 `driver_init` 再到 DRR recovery 的链路需要在模块文档中完整串起来。
3. **文档分层有利于实现**: `RE/驱动模块ABI设计.md` 作为完整规范，`CODE/DKM/README.md` 作为模块落地说明，职责清晰。

### 教训

1. **不要把 DKM 写成泛泛说明**: 驱动模块系统涉及启动顺序、ELF、ABI、UTSM、DRR，README 必须明确边界和第一版范围。
2. **早期加载限制要写清**: stage0/stage1 不能依赖 VFS 和普通 heap，否则启动链会循环依赖。
3. **实现清单要保留**: 后续写代码时应按 manifest parser、boot module lookup、ELF loader、relocation、driver state table 的顺序推进。

## 2026-06-29 — 接入 DKM 内置驱动最小链路

### 经验

1. **先内置再外置**: 在实现外部 ELF `.drv` loader 前，先用内置 `console_early` 驱动验证 `driver_desc -> driver_init -> ACTIVE`，能降低 DKM 首次接入风险。
2. **kernel_api 是驱动边界**: 即使内置驱动也通过 `dkm_kernel_api` 调用 log 服务，能保持未来外部驱动 ABI 一致。
3. **验证点要清晰**: 串口输出 `[DKM] builtin driver active` 和 `[DKM:console_early] serial console online`，说明 ABI 校验、handle 创建和 init 调用路径已跑通。

### 教训

1. **不要直接跳到外部 ELF loader**: manifest parser、boot module、relocation、符号解析同时实现会放大调试范围，应先固定 ABI 和状态机。
2. **外部 `.drv` 仍未执行**: 当前 `SYSTEM/driver` 下的 `.drv` 只是镜像内容，必须实现 DSM/ELF loader 后才会被加载。
3. **内置驱动也应走同一状态机**: 避免未来外部加载器和内置路径行为不一致。

## 2026-06-29 — 接入 Limine boot module 与 DKM ELF 符号扫描

### 经验

1. **先识别再加载**: 外部 `.drv` 应先通过 boot module 枚举和 ELF64 header 校验，再解析 section table / symtab / strtab，最后定位 `driver_desc` / `driver_init` / `driver_exit`。
2. **Limine request 需要 linker 保留**: module request 必须放入 `.limine_requests*` 区段，并在 linker script 中显式保留 start/request/end，否则 bootloader 不会返回 response。
3. **.drv 是 ET_REL 可重定位文件**: 当前 stub 驱动编译为 `-r` 可重定位目标文件，附带了 `.symtab` / `.strtab` 和完整符号表，内核可以直接遍历 symtab 找到目标符号的 section 和 offset。
4. **streq 足够**: 内核内联 `streq("driver_desc")` 已足够定位所需符号，不需要 hash 或完整的字符串匹配库。

### 教训

1. **module 配置要在 limine.conf 中显式声明**: 仅把 `.drv` 放进 `SYSTEM/driver` 不会被 bootloader 自动预加载。
2. **有符号表 ≠ 可调用**: 符号找到了，但 ET_REL 没有 load address / vaddr，relocation 未完成前不能直接跳转。
3. **boot module 适合 stage0/stage1**: 文件系统未就绪前，早期驱动应优先通过 boot module 进入内核。

## 2026-06-29 — 实现 ET_REL 外部驱动装载与 relocation

### 经验

1. **section offset + image base 才能得到正确地址**: ET_REL relocation 中符号值为 section 内偏移，重定位写入的目标地址必须是 `image_base + section_offset + symbol_value + addend`，只算 offset 会导致低地址指针。
2. **NX 是早期加载器的隐性障碍**: arena 位于 `.bss`，默认只有 PF_R|PF_W，调用 `driver_init` 需要可执行内存。SAS-R0 下通过 linker script PHDRS 给 data 段加 PF_X 是最简方案。
3. **Et_REL 只需 R_X86_64_64 即可起步**: 当前 stub driver 只有三种字符串指针重定位，全部是 R_X86_64_64，loader 复杂度极低。

### 教训

1. **不能只看 ELF dump 输出验证重定位**: 必须用 hex 指针输出确认运行时地址落在正确区间，否则空白输出难以区分是空字符串还是 NULL 指针。
2. **arena 分配的内存在真实硬件上可能不可执行**: QEMU 模拟器不严格检查 NX 时不会暴露问题，但 Limine 的 page table 会强制 NX，未来应在 loader 中分配单独的可执行区域。
3. **PHDRS 合并 .data/.bss 前要确认 startup 代码没有依赖 .bss 零初始化边界**: 当前 boot.S 没有依赖，合并安全。

## 2026-06-29 — 实现 manifest.json 解析与分阶段驱动加载

### 经验

1. **最小 JSON parser 足以处理 manifest**: 只需要 tokenizer + 字符串读取 + 递归对象/数组跳过，无需完整 JSON 库。
2. **Limine 路径有前导 `/`**: `boot():/driver/...` 返回的路径以 `/` 开头，manifest 中的路径 `driver/...` 不含前导 `/`，匹配时需要 strip。
3. **按 stage 分阶段加载天然隔离故障**: platform → boot → filesystem → optional，每 stage 内部 required 驱动失败只终止当前 stage，不阻断后续 stage 的 optional 驱动。

### 教训

1. **JSON value skipper 必须处理所有类型**: 对象 `{}`、数组 `[]`、字符串、数字、布尔，缺一不可，否则 parser 在遇到未知 value 类型时 break。
2. **PowerShell Set-Content 会写 BOM**: 对 C 源文件应避免使用，否则 BOM 字符 `U+FEFF` 在文件开头导致 clang 报错 `extraneous closing brace`。
3. **arena 不够 14 个驱动**: 当前 4MB arena 在加载全部 14 个驱动后已接近容量上限，后续需要实现真正的物理页分配器。

## 2026-06-29 — 实现第一个真实 DKM 驱动：PIT timer

### 经验

1. **真实驱动需要 BSS**: 驱动中声明 `static const struct dkm_log_api *g_log` 会产生 `.bss` section（SHT_NOBITS + SHF_ALLOC），loader 必须为其分配零填充空间。
2. **R_X86_64_PC32 和 R_X86_64_32S 是代码重定位主流**: inline asm 的 `outb/inb` 使用立即数端口，编译器生成 PC32 对 `.bss` 的引用和 32S 对 `.rodata.str1.1` 的引用。
3. **PIT 校准可在无 IRQ 下工作**: 通过 latch command 读取 PIT 当前计数值，在 mode 0 terminal count 后计数值回绕，可做精确的 busy-wait 延迟。

### 教训

1. **driver_desc 不能 static**: ELF 符号 `driver_desc` 必须是 global（`visibility("default")`），否则 symtab 中符号为 LOCAL，loader 找不到。
2. **真实驱动的 .text 有 `R_X86_64_PC32` 重定位**: stub 驱动的 `driver_init` 只是 `xor eax,eax; ret` 没有重定位，但真实的 `outb/inb` 内联汇编会产生 PC32 对 g_log 全局变量的引用。

## 2026-06-29 — 修复 UTSM 早期启动崩溃

### 经验

1. **先验证崩溃阶段**: 串口只到 `[UTSM] boot` 时，实际还未进入 DKM 驱动加载，不能把早期 runtime 崩溃误判成缺真实驱动。
2. **早期日志不能无限等待硬件**: `serial_putc()` 对 COM1 ready 位无限 busy-wait 会让内核卡死，early serial 必须有超时或降级策略。
3. **未初始化 FPU/SSE 前禁止编译器生成 SIMD 指令**: Clang 即使在 freestanding 下也可能生成 `xorps/movups`，早期内核未启用 SSE 会触发异常，应使用 `-mno-sse -mno-sse2 -mno-mmx -msoft-float`。

### 教训

1. **进入 C 前要固定 CPU 状态**: `_start` 应先 `cli` 和 `cld`，否则未建 IDT 前被中断或方向标志异常会导致三重故障。
2. **驱动不能直接下载复用**: Linux/Windows 已编译驱动不符合 DKM ABI，真实驱动必须按 Deshab 的 `driver_desc/driver_init/kernel_api` 原生实现。
3. **stub `.drv` 只是镜像样本**: 在 DSM manifest parser、ELF loader、relocation 和 `dkm_kernel_api` 实现前，`SYSTEM/driver` 下的 `.drv` 不会被内核加载执行。

## 2026-06-29 — 编写 UTSM 内核代码首阶段骨架

### 经验

1. **先闭环再扩展**: 先实现 Limine 入口、serial 日志、UTSM init、segment 创建、UTRW read/write 和 selftest，可以快速把设计变成可验证路径。
2. **保持 64B line 接口**: 即使第一版使用占位 XOR stream，也保留 cache line 级加解密接口，后续替换真实加密不会影响 UTRW 主路径。
3. **构建脚本要先检查工具链**: build.ps1 明确检查 clang、ld.lld、make，能快速定位环境缺失，而不是让 make 报模糊错误。

### 教训

1. **Windows make 会受环境影响**: MinGW make 会把 `CC` 默认为 `cc`，Makefile 中应强制指定 `CC := clang`，不能只用 `?=`。
2. **当前环境缺少 LLVM 工具链**: 本机 PATH 中没有 clang 和 ld.lld，当前无法生成 `SYSTEM/boot/utsm.elf`，需要先安装 LLVM/Clang 或提供交叉工具链。
3. **第一版不要引入完整系统复杂度**: DRR、DKM、真实 checkpoint、buddy/slab 和多 CPU 调度都应在 selftest 闭环后逐步接入。

## 2026-06-29 — 自动发现 LLVM 并成功构建 UTSM

### 经验

1. **工具链可自动发现**: LLVM 安装在 `C:\Program Files\LLVM\bin` 但不在 PATH 时，build.ps1 可以扫描常见目录并把绝对路径传给 Makefile。
2. **高半区内核要使用 kernel code model**: 链接地址位于 `0xffffffff80000000` 时，CFLAGS/ASFLAGS 需要 `-mcmodel=kernel`，否则会出现 `R_X86_64_32 out of range`。
3. **清理旧 object 很关键**: 修改编译参数后必须先 clean，否则旧 object 仍按旧 code model 生成，链接错误会继续存在。

### 教训

1. **不要只检查 PATH**: Windows 上 LLVM 可能已安装但没写入 PATH，构建脚本应扫描常见安装目录。
2. **PowerShell 路径含空格要传绝对参数**: `C:\Program Files\LLVM\bin\clang.exe` 这类路径应通过 make 变量传递，避免 shell 展开错误。
3. **MinGW make 的中文临时目录警告可忽略但需关注**: 当前 `TMP/TEMP Illegal byte sequence` 未阻断构建，但后续若引发临时文件问题，应在 build.ps1 中设置 ASCII 临时目录。

## 2026-06-29 — 打包 SYSTEM 为 IMG 并添加 QEMU 启动脚本

### 经验

1. **不依赖外部镜像工具也能先闭环**: build.ps1 直接生成 FAT16 raw IMG，可以在没有 mtools/oscdimg/xorriso 的环境下把 SYSTEM 打包出来。
2. **构建脚本应负责完整产物链**: 现在 `build.bat -> build.ps1 -> utsm.elf -> ISO/deshab.img` 形成了连续流程。
3. **QEMU 启动脚本应自动找程序**: `ISO/run_qemu.bat` 会从 PATH 和常见安装目录找 `qemu-system-x86_64.exe`，使用 `-serial stdio` 方便看内核串口日志。

### 教训

1. **IMG 命名不等于 ISO 文件系统**: 当前产物是 FAT16 raw 磁盘镜像 `deshab.img`，目录名叫 ISO 只是输出目录，后续若需要真正 ISO9660 应单独实现。
2. **Limine 是否能从该 IMG 启动仍需 QEMU 验证**: 镜像已包含 EFI/BOOT/BOOTX64.EFI、limine.conf 和 utsm.elf，但最终启动链需要实际运行 QEMU 检查。
3. **临时构建文件需要过滤**: 打包 SYSTEM 时应排除 `*.tmp*`，避免旧的 `utsm.elf.tmp...` 被写入镜像。

## 2026-06-29 — 修正 UEFI 镜像为 GPT + FAT32 ESP

### 经验

1. **UEFI 磁盘镜像应有 GPT**: 仅裸 FAT 分区不等于可启动 UEFI 磁盘，镜像需要保护 MBR、GPT 主/备表和 ESP 分区。
2. **ESP 应使用 FAT32**: QEMU/UEFI 固件对 ESP 的预期是 FAT32 文件系统，`EFI/BOOT/BOOTX64.EFI` 应位于该分区内。
3. **可直接验证关键签名**: 构建后检查 MBR `AA55`、保护分区类型 `EE`、GPT 签名 `EFI PART`、FAT32 标记，能快速确认镜像结构正确。

### 教训

1. **裸文件系统镜像不够**: 之前的 FAT16 raw 镜像缺少分区表，UEFI 固件不能按磁盘启动语义识别。
2. **PowerShell 数字字面量要注意有符号转换**: `0xffffffff` 在某些上下文会变成 `-1`，应使用 `4294967295` 或显式 UInt64。
3. **run_qemu 必须显式使用 UEFI 固件**: 仅 `qemu-system-x86_64 -drive ...` 不保证走 UEFI，需要加载 `edk2-x86_64-code.fd` 或 `OVMF_CODE.fd`。

## 2026-06-29 — 实现 6 个真实 DKM 驱动（timer/acpi/pci/console_fb/bootfs/vfs）

### 经验

1. **kernel_api 渐进暴露平台能力**: rsdp_address → fb_address → boot_modules_response，按需添加字段避免了 ABI 大范围修改。
2. **驱动独立编译必须保持 struct 布局一致**: 驱动本地定义的 `dkm_kernel_api` 必须包含所有字段（含 padding），否则偏移量错位导致读取错误。
3. **Limine framebuffer 让早期图形可行**: 通过 Limine framebuffer request 获取线性帧缓冲，在无 GPU 驱动下即可实现 8x8 字体渲染。

### 教训

1. **PCI 扫描不需要 IRQ**: 纯配置空间枚举完全独立于中断系统，pci 驱动可以第一个实现。
2. **bootfs/vfs 是软件驱动，不依赖硬件**: 利用 Limine boot module 可直接构造内存文件系统，无需块设备。
3. **console_fb 的 bpp/pitch 必须从 bootloader 读取**: 帧缓冲格式随 QEMU/固件变化，硬编码会导致花屏。

## 2026-06-30 — 记录 devfs/fat32/IDT/IRQ/ps2kbd/e1000 阶段成果

### 经验

1. **先做内核 IRQ 底座再写中断驱动**: IDT 0–47 stub、PIC remap、统一 `idt_handler` 和 `irq_register` 先闭环后，`ps2kbd` 与 `e1000` 都能通过同一 IRQ 注册路径接入。
2. **HHDM 是早期 MMIO 的最短路径**: 通过 Limine HHDM request 暴露 `kernel_api.hhdm_offset`，e1000 可直接把 PCI BAR0 物理地址映射到高半区虚拟地址，避免在页表/MMIO allocator 未完善前阻塞网卡初始化。
3. **软件文件系统可先用 boot module 验证**: devfs 不依赖块设备，fat32 可用 `driver/test.fat32` 合成镜像验证 BPB/FAT/目录项/文件读取，为后续 AHCI/NVMe 块设备接入保留接口经验。
4. **当前真实驱动进入 10/14 阶段**: 已完成 timer、acpi、pci、console_fb、bootfs、vfs、devfs、fat32、ps2kbd、e1000，剩余真实驱动集中在 APIC、块设备和 virtio_net。

### 教训

1. **汇编文件命名会影响 object 冲突**: `idt.S` 和 `idt.c` 同名会同时输出 `idt.o`，应将汇编入口拆成 `idt_stubs.S`，避免覆盖或重复符号。
2. **中断 stub 宏要显式区分 error code**: x86_64 异常中部分 vector 自动压入 error code，stub 生成不正确会破坏统一栈帧，导致 `iretq` 返回失败或异常连锁。
3. **驱动本地 ABI struct 必须同步**: `irq_register`、`hhdm_offset` 等字段加入后，所有真实驱动本地 `dkm_kernel_api` 定义都必须保持相同偏移，否则会读取到错误函数指针或平台地址。
4. **QEMU 过滤日志可能隐藏有效输出**: FAT32 README 内容、VFS dump、e1000 reset warning 等行如果没有统一前缀，使用 Select-String 时可能被漏看；驱动日志应带稳定模块前缀。
5. **e1000 reset timeout 暂不等于驱动失败**: 当前已能完成 PCI 探测、MAC 读取、IRQ 注册和 link setup，reset timeout 需要后续结合硬件状态轮询/RX-TX 初始化继续收敛。

## 2026-06-30 — 实现 APIC discovery 驱动

### 经验

1. **APIC 应先 discovery 后接管**: 当前内核 IRQ ack 仍走 PIC EOI，stage0 `apic` 先做 CPUID、`IA32_APIC_BASE`、MADT、LAPIC MMIO 只读探测，并继续保留 PIC 路由，能避免破坏现有 `ps2kbd/e1000` IRQ 路径。
2. **MADT 可不依赖 ACPI 驱动**: `apic` 在 manifest 中早于 `acpi`，因此直接使用 `kernel_api.rsdp_address` 解析 RSDP/XSDT/MADT，枚举 LAPIC、IOAPIC 和 interrupt source override。
3. **HHDM 适合只读 LAPIC 验证**: 通过 `kernel_api.hhdm_offset + lapic_phys` 可读取 LAPIC ID/version/SVR，验证 MMIO direct map 可用于平台控制器探测。
4. **QEMU 验证链路明确**: 日志显示 `MADT found`、`IOAPIC addr=0xfec00000`、`LAPIC version reg=0x50014`、`discovery complete; PIC routing retained`，随后 `pci` 继续加载并 `UTSM SELFTEST PASS`。

### 教训

1. **不要提前切换 IOAPIC**: 未扩展 IDT/vector allocator/APIC EOI 前，不能禁用 PIC、重编 IOAPIC RTE 或启用 LAPIC timer，否则中断投递和 EOI 后端会不一致。
2. **驱动也要用 kernel code model**: APIC 驱动代码较大时，Clang 会生成更多字符串地址 relocation；使用普通 code model 会让部分高半区字符串日志异常，需用 `-mcmodel=kernel` 编译真实 `.drv`。
3. **APIC required 驱动不能误失败**: `apic` 是 stage0 required 且为 `pci` 提供 `irq` 依赖，除非 `api/log/irq_register` 缺失，否则 discovery 不完整也应返回 0 并保持 PIC fallback。

## 2026-06-30 — 实现 AHCI discovery 驱动

### 经验

1. **块设备先枚举再 DMA**: AHCI 第一版只做 PCI class 探测、BAR5/ABAR HHDM 映射、HBA CAP/GHC/PI/VS 和 port 寄存器枚举，不发命令，能在无 DMA allocator/页固定接口前安全验证控制器路径。
2. **AHCI 可作为 block provider 先接入**: `ahci` 是 stage1 optional，依赖 `pci/irq` 并提供 `block`，即使还未提供真实读写，也能让 manifest/VFS/fat32 依赖链进入下一阶段测试。
3. **QEMU ICH9 AHCI 验证稳定**: QEMU 下探测到 `8086:2922`，ABAR 位于 `0x81084000`，PI 为 `0x3f`，6 个 implemented ports 可读，port 2 显示 ATAPI signature `0xeb140101`。

### 教训

1. **PCI class dword 位移容易错**: 读取 offset `0x08` 时，prog_if 在 bits 15:8，subclass 在 bits 23:16，class_code 在 bits 31:24；按 byte offset 直觉解析会导致 AHCI 控制器找不到。
2. **不要提前清端口/发命令**: 未建立 command list/FIS/PRDT DMA 内存、cache flush 和中断完成路径前，不能贸然 stop/start port 或写 PxCI。
3. **HHDM MMIO 只读仍要保守**: 写 PCI command 只启用 MEM/BUSM，AHCI HBA 寄存器阶段保持只读，避免破坏固件/仿真器当前端口状态。

## 2026-06-30 — 实现 NVMe discovery 驱动

### 经验

1. **NVMe 也应先 discovery**: 第一版只做 PCI class `01/08/02` 探测、BAR0/BAR1 解析、IRQ line 读取，不创建 admin queue、不写 CC、不提交命令，能安全接入 stage1 storage 链路。
2. **测试设备需显式加入 QEMU**: 默认启动脚本只挂 virtio 磁盘，不会出现 NVMe 控制器；临时加入 `-device nvme` 后可探测到 QEMU NVMe `1b36:0010`。
3. **高位 BAR 要主动保护**: QEMU 将 NVMe BAR0 放到 `0x000000c000004000`，当前 HHDM/页表路径不能安全访问该 4G 以上 PCI MMIO，因此驱动记录 BAR 后跳过 register read，系统仍能 SELFTEST PASS。

### 教训

1. **不能假设所有 BAR 都在 4G 以下**: e1000/AHCI 的低位 BAR 可直接 HHDM 读取，但 NVMe 常见为 64-bit BAR，需要后续实现高位 PCI MMIO 映射或页表映射接口。
2. **MMIO 读前必须检查映射能力**: 直接读未映射高位 BAR 会导致异常或 QEMU 退出，真实驱动必须在访问寄存器前验证地址范围和映射策略。
3. **NVMe admin queue 不能提前做**: 未准备 contiguous DMA、doorbell stride、completion polling/IRQ 和 cache coherency 前，不应写 CC.EN 或 ASQ/ACQ。

## 2026-06-30 — 实现 virtio_net discovery 驱动并完成 14/14 真实驱动

### 经验

1. **virtio-net 可先做 capability discovery**: 第一版只识别 PCI vendor/device、BAR、IRQ line 和 modern virtio PCI capabilities，不做 feature negotiation、不建 virtqueue，能安全完成网络驱动入口闭环。
2. **兼容 transitional/modern 识别**: QEMU `virtio-net-pci` 以 `1af4:1001` 形式出现，按 virtio transitional ID 计算 virtio_id=1 即可识别 net device，同时仍能枚举 modern capability 链。
3. **14 个外部 `.drv` 全部真实化**: manifest 中 timer、apic、acpi、pci、console_fb、ahci、nvme、bootfs、vfs、fat32、devfs、e1000、virtio_net、ps2kbd 均已替换为可执行 DKM 驱动。

### 教训

1. **测试设备要显式挂载**: 默认 QEMU 脚本没有 virtio-net，需要临时添加 `-netdev user` 和 `-device virtio-net-pci` 才能验证驱动路径。
2. **virtio capability 链只应只读枚举**: 未建立 DMA ring 和设备状态机前，不应写 common_cfg/device_status 或 queue_select/queue_enable。
3. **多个网卡测试会改变 PCI 拓扑**: 加入 virtio-net 后 e1000 可能不存在或位置变化，驱动必须把“设备未找到”视为 optional 正常路径。

## 2026-06-30 — 规划 14/14 驱动完成后的下一阶段路线

### 经验

1. **先补底座再做数据路径**: 14 个驱动已完成 discovery/加载闭环，下一步最优先不是继续堆功能，而是补物理页分配、页表/MMIO 映射、DMA buffer 和屏障约定。
2. **块设备应先于文件系统深化**: AHCI/NVMe 的 IDENTIFY/READ 能把 FAT32 从 `test.fat32` 迁移到真实 block provider，是后续 VFS 能力扩展的关键路径。
3. **IRQ 后端需要独立升级**: 现有 PIC fallback 已稳定，APIC/IOAPIC/MSI 应作为单独阶段推进，避免在设备数据路径中同时改变中断后端。

### 教训

1. **不要在 discovery 驱动里提前发命令**: AHCI/NVMe/virtio/e1000 都需要 DMA、队列、IRQ 完成路径；缺少底座时发命令会把问题混在一起。
2. **高位 PCI MMIO 是必须解决的问题**: NVMe BAR0 已暴露 4G 以上映射缺口，后续不能继续依赖简单 HHDM 假设覆盖所有设备 MMIO。
3. **文档状态要及时修正**: CLAUDE.md 曾残留旧的“待实现 nvme/virtio_net”尾部，完成阶段性目标后必须同步路线，避免后续开发误判当前状态。

## 2026-06-30 — 编写 DSK 主内核 README

### 经验

1. **明确两阶段内核边界**: `utsm.elf` 是 bootloader 加载的首阶段内核，`deshab.elf` 是 DSK 主内核；文档中必须明确 UTSM 后续要继续执行到 DSK。
2. **先定义 boot context**: UTSM 跳转 DSK 前应通过版本化 `dsk_boot_context` 传递 HHDM、RSDP、framebuffer、boot modules、DKM API/driver table、UTSM/DRR 状态等信息。
3. **先做最小 DSK 闭环**: DSK 第一版只需接收 context、打印 boot banner、执行 selftest，即可验证 `CODE/dsk -> SYSTEM/system/deshab64/deshab.elf` 的链路。

### 教训

1. **不要让 DSK 重复早期启动职责**: Limine request、early serial、DKM 首轮加载和 discovery 应保留在 UTSM 阶段，DSK 接管长期运行期。
2. **ELF loader 第一版要收窄范围**: DSK loader 应先只支持 ELF64、PT_LOAD、入口跳转，不引入动态链接、TLS、demand paging 或符号解析。
3. **构建路径要固定**: 主内核产物固定为 `SYSTEM/system/deshab64/deshab.elf`，后续 build.ps1 和 limine/bootfs 逻辑都应围绕该路径实现。

## 2026-06-30 — 实现 UTSM 到 DSK 最小跳转闭环

### 经验

1. **先用 FAT32 路径预加载闭环**: `deshab.elf` 已放入 `SYSTEM/system/deshab64/`，Limine 从 FAT32 ESP 预加载，UTSM 按 `/system/deshab64/deshab.elf` 定位模块并加载，符合后续替换为真实 FAT32/block provider 的路径语义。
2. **最小 ELF loader 足够验证架构**: 第一版只支持 ELF64、ET_DYN/ET_EXEC、PT_LOAD、一次性复制/清零和入口跳转，已经能从 UTSM 进入 DSK 并传递 context。
3. **DSK 不返回符合主内核语义**: DSK 校验 `dsk_boot_context` 后输出 HHDM/RSDP/framebuffer/boot modules/DKM API 等信息，`SELFTEST PASS` 后 halt，完成主内核接管模型验证。

### 教训

1. **构建脚本要显式集成 DSK**: build.ps1 必须同时构建 `CODE/UTSM` 和 `CODE/dsk`，并检查 `SYSTEM/system/deshab64/deshab.elf` 是否存在，否则镜像中不会包含主内核。
2. **PowerShell/MinGW make 路径状态要谨慎**: `Push-Location`/`make -C` 在当前环境中出现路径解析问题，最终采用显式 `$DskDir` 与 make 工作目录方式稳定构建。
3. **当前还不是真实 FAT32 自读取**: 现在 UTSM 通过 Limine module 拿到 `deshab.elf`，下一步要把来源替换为 AHCI/NVMe block provider + FAT32 读取。

## 2026-06-30 — 建立 DMA API 并验证 AHCI IDENTIFY/READ

### 经验

1. **DMA 底座先从低位页开始**: 通过 Limine memmap 选取 4G 以下 usable 区间，配合 HHDM 形成 `phys/virt/size` 三元组，足以支撑 AHCI CLB/FIS/command table/data buffer。
2. **kernel_api.dma 是块设备公共入口**: 将 `dkm_dma_api.alloc_pages` 暴露给 DKM 后，AHCI 不再依赖私有内存假设，后续 NVMe/e1000/virtio-net 也可以复用。
3. **AHCI 最小数据路径已跑通**: 挂临时 SATA 测试盘时，AHCI 能完成 SATA disk IDENTIFY，并执行 READ DMA EXT 读取 LBA0；默认无 SATA 盘时会跳过命令并继续进入 DSK。

### 教训

1. **AHCI command header 布局必须严格按规范**: 初版把 `prdtl/prdbc` 字段布局写错，导致 HBA 看到错误 PRDT 信息，IDENTIFY 一直超时。
2. **不要对 ATAPI/空端口发 ATA IDENTIFY**: 默认 QEMU 场景只有 ATAPI/空端口时应跳过命令，否则会产生误导性 timeout。
3. **测试盘场景要和默认启动分开**: 默认 `run_qemu.bat` 保持稳定启动；SATA 测试盘用于验证 AHCI 数据路径，后续可整理成独立自动化场景。

## 2026-06-30 — 接入 block provider 并让 FAT32 优先读真实块设备

### 经验

1. **block provider registry 解耦驱动和文件系统**: UTSM 提供 `kernel_api.block.register_device/read` 后，AHCI 只负责注册 `ahci0` 和实现 LBA read，FAT32 不需要知道 AHCI 细节。
2. **保留 boot module 回退很关键**: 默认 QEMU 没有 SATA disk 时，FAT32 会继续读取 `driver/test.fat32`，保证启动链和 DSK 跳转不受真实块设备实验影响。
3. **SATA FAT32 镜像验证了真实路径**: 把 `test.fat32` 扩展成 SATA 测试盘后，AHCI 注册 block provider，FAT32 通过 block provider 读取 BPB/root/README.TXT，并继续进入 DSK。

### 教训

1. **ABI 字段只能追加不能插入**: `dkm_kernel_api.block` 必须追加到结构尾部，不能插在 `dma` 后面，否则会改变已有驱动本地 ABI 字段偏移。
2. **block read 需要支持多扇区分块**: AHCI 当前 command table 只有一个 PRDT entry，公共 read 回调内部按最多 8 sector 分块，避免一次请求过大。
3. **FAT32 应先校验 BPB 再切换来源**: block provider 读取成功但 BPB 无效时必须回退 boot module，否则普通测试盘会让 FAT32 误判失败。

## 2026-06-30 — DSK 加载：优先 FAT32 block provider，回退 Limine module

### 经验

1. **两阶段加载优先级清晰**: DSK loader 先尝试 `kernel_api.block` + 内嵌 FAT32 解析，读取根目录 `DESHAB  ELF` 文件；无块设备或文件不存在时自动回退 Limine boot module，保证默认场景稳定。
2. **FAT32 data_start 必须乘以 fat_count**: `data_start_sec = rsvd + fat_count * spf`，漏掉第二份 FAT 会导致 root directory cluster 错位。
3. **静态 BSS 缓冲区比 arena 分配更可靠**: DSK loader 的 64KB BPB/FAT 缓冲区、4KB cluster buffer、64KB file data buffer 使用 BSS 静态数组，绕过了 UTSM arena 4MB 限制，并已扩大 arena 到 16MB。

### 教训

1. **arena 容量需提前评估**: 原 4MB arena 在加载 14 个 DKM 驱动后剩余空间不足，导致 `kmem_alloc_aligned(64KB)` 失败，已调整为 16MB。
2. **AHCI 单扇区 block read 在重入时可能不稳定**: batch read 128 扇区总是成功，但单扇区连续读偶尔超时；DSK loader 最终全部改用 cluster 级批量读取解决。
3. **调试时 hex dump 比 ASCII 日志可靠**: FAT32 目录项包含空格，ASCII 日志容易遗漏；用 `log_hex64` 输出原始字节准确定位了 `data_start_sec` 偏移错误。


## 2026-06-30 — DSK 首次启动链路 + FirstInit 用户设置向导 + mouseInit

### 经验

1. **PIE ELF 必须处理 R_X86_64_RELATIVE**: DSK 用 `-pie` 编译 FirstInit/mouseInit，加载后所有全局指针/数组引用需要重定位。在 `dsk_load_elf` 中解析 `PT_DYNAMIC` → `DT_RELA/DT_RELASZ` → 应用 `R_X86_64_RELATIVE`（`*slot = load_bias + addend`），否则解引用全局数据会 page fault。
2. **PIE 指针数组需改用栈上字符数组**: DSK 自身的 `static const char *fi_txt_path[]` 含 `R_X86_64_RELATIVE` 重定位项，但 UTSM loader 不处理 `.rela.dyn`。改用栈上 `char[]` 逐字节赋值避免重定位依赖。
3. **`__attribute__((unused))` 在 -O2 下可能导致函数被优化掉**: `fb_char`/`fb_text` 标记为 unused 后，编译器可能内联消除或改变调用约定，导致按键时崩溃。去掉 unused 属性即可修复。
4. **FAT32 文件缓冲区必须足够大**: FirstInit.elf 从 26KB 增长到 88KB 后超过 DSK 的 `g_fdata[65536]`，导致加载失败。扩大到 256KB 解决。
5. **PS/2 控制器写命令在 QEMU 早期阶段可能阻塞**: mouseInit 的 `ps2_write_cmd` 写 `0x64` 端口后 `wait_input_clear` 死循环。安全 stub 版本只读状态寄存器，不写控制器。
6. **预渲染中文位图是最简字体方案**: 用 Python/Pillow 从 simhei.ttf 渲染指定中文字符串为 8bpp 灰度 C 数组，嵌入 ELF，避免运行时 TTF 解析。

### 教训

1. **ELF 文件大小监控**: 新增功能后 ELF 体积可能超过静态缓冲区。应在 `dsk_load_elf` 中校验 `found_size <= sizeof(g_fdata)` 并打印日志。
2. **圆角矩形绘制需完整边界检查**: `draw_card` 的圆角填充如果缺少 `sq <= 0` 检查，`isqrt_int` 会收到负数转无符号的大值，导致越界写入。
3. **帧缓冲 blend 不应读取 framebuffer**: `blend(line[c], fg, a)` 读取帧缓冲可能触发 page fault 或性能问题。改为 `blend(bg, fg, a)` 纯写入。
4. **block API 偏移量必须精确**: `dkm_kernel_api` 中 `block` 字段在 `0xA8`（168），之前误用 `0x60`（96 = `rsdp_address`），导致 `g_block_read` 永远为 NULL。
5. **AHCI 单扇区读不稳定**: 批量读 256 扇区成功，但回退到单扇区读会挂死。DSK 的 `fat32_read_root_file` 应尽量在 256 扇区缓冲内完成所有操作。

### 当前已完成

```text
启动链路:
  Limine → utsm.elf → DKM 14 驱动加载 → DSK (deshab.elf)
    → 旋转加载动画 (双缓冲, 顺时针, comet-tail)
    → FAT32 读取 firstInit.txt
    → firstInit=0:
        DSK → mouseInit.elf (安全 stub) → 返回 DSK
        DSK → FirstInit.elf
          → 环淡出 → "欢迎使用 Deshab" → 设置提示
          → 账户设置卡片 (圆角矩形, 白色输入框, 18px 中文)
          → 键盘输入: 计算机名/用户名/密码
          → SHA256 密码 + XOR 加密配置缓冲
          → "设置完成" 提示
    → firstInit≠0: 正常启动 (待实现)

字体系统:
  CODE/font/mkfont.py → simhei_16/24/32.dbf (DBF 格式)
  CODE/font/render_embedded.py → text_bitmaps.c (预渲染位图)
  SYSTEM/system/font/simhei.ttf (原始 TTF)

初始化程序架构:
  DSK (deshab.elf) 负责调度所有系统初始化程序
  mouseInit.elf — DSK 直接加载调用, 返回 DSK
  FirstInit.elf — DSK 加载跳转, 只做用户设置向导
```

### 下一步计划

1. **修复 FirstInit 按键崩溃**: 定位 `fb_text` 在 `-O2` 下的优化问题（已移除 unused 属性，待验证）
2. **实现鼠标光标移动**: 从 PS/2 IRQ12 读取鼠标包，更新光标位置
3. **user.conf 写盘**: 需要 `dkm_block_api.write` + AHCI write DMA + FAT32 文件覆写
4. **网络配置界面**: FirstInit 第二步，IP/DHCP/网关设置
5. **FirstInit 完成后跳转**: 设置完成后写 firstInit=1，跳转正常启动路径
6. **block write API**: 在 `dkm_block_api` 中增加 `write` 函数，AHCI 实现 WRITE DMA
7. **FAT32 write 支持**: 文件创建、目录项写入、FAT 链更新
8. **内存与 MMIO 基础设施**: 物理页分配器、页表映射、高位 PCI MMIO
9. **IRQ 后端升级**: APIC EOI/IOAPIC/MSI-X
10. **网络数据路径**: e1000 RX/TX, virtio-net virtqueue

## 2026-06-30 — 优化 FirstInit framebuffer 首次启动 UI

### 经验

1. **渐变背景需要统一擦除路径**: DSK spinner 与 FirstInit 共享 `BG_TOP/BG_BOTTOM` 语义，并通过按 y 计算背景色的局部擦除函数维护一致背景，可避免动画/转场留下纯色块。
2. **圆角矩形应只绘制有效区域**: 直接逐像素判断 rounded-rect coverage，只写圆角范围内的像素，比“先填满矩形再擦角”更适合卡片叠层和输入框，能避免角落脏色块。
3. **嵌入字体尺寸可独立调优**: 将 `render_embedded.py` 的字号从 32px 降到 24px 后，`text_bitmaps.c` 中 welcome 位图从 240x38 缩到 180x30，卡片在 1280x800 内更轻盈。

### 教训

1. **帧缓冲 sprite 也要匹配底层背景**: DSK spinner 的离屏 sprite 若仍用纯色 bg 初始化，即使屏幕先画了渐变，也会在 spinner 方块区域破坏渐变连续性。
2. **构建产物可能被 QEMU 锁定**: `ISO/deshab.img` 被正在运行的 `qemu-system-x86_64.exe` 占用时，build.ps1 可完成编译/链接但会在 GPT IMG 打包阶段失败；重新打包前需关闭 QEMU。
3. **Python 环境不等于有 Pillow**: 系统只有 WindowsApps python stub，Blender/WeCom 自带 Python 也缺 PIL；可临时使用 Blender Python + `.build_tmp/pydeps` 安装 Pillow 生成字体位图，避免污染项目源码。

## 2026-07-01 — 添加 netman 联网器与网络配置加载

### 经验

1. **系统初始化器应由 DSK 调度**: `netman.elf` 与 `mouseInit.elf` 一样作为系统组件，由 `deshab.elf` 直接从 FAT32 加载调用，避免 FirstInit 接管系统初始化职责。
2. **早期配置要有扁平 8.3 兜底**: 用户路径保留为 `SYSTEM/system/deshab64/network/conf/conf.conf`，但 SATA 测试镜像额外写入 root 目录 `NETCONF.CNF`，方便早期 loader 在没有 VFS 路径解析时读取。
3. **先做配置管理闭环**: 在 `kernel_api.net=0`、e1000/virtio-net 仍缺 RX/TX 数据路径时，先实现 netman 配置解析、状态输出和启动调度，为后续 net API/协议栈接入留下稳定入口。

### 教训

1. **完整联网不能只写 UI**: FirstInit 的网络设置页只产生配置，真正联网还必须补 `kernel_api.net`、netdev 注册、NIC RX/TX、ARP/IP/UDP/DHCP/DNS。
2. **打包工具也要同步编译**: 修改 `.build_tmp/mkfat32.c` 后必须重新生成 `mkfat32.exe`，否则 build.ps1 会继续使用旧打包逻辑，导致新 ELF/配置未进入 SATA 镜像。
3. **早期读取文件要限制格式**: netman 第一版读取 root 8.3 文件，避免在没有完整 VFS/FAT32 路径遍历前引入复杂目录解析和写盘风险。

## 2026-07-01 — 实现最小 net API 与 netdev 注册查询闭环

### 经验

1. **新增 ABI 能力优先复用保留槽位**: `dkm_kernel_api.net` 原本就是占位字段，只把类型从 `const void *` 收窄为 `const dkm_net_api *` 并赋值 `net_get_api()`，不在其前方插入字段，可保持 net 偏移稳定。
2. **网络底座先用固定表**: 早期驱动按 stage 顺序单线程加载，固定 8 槽 netdev registry 具备 O(1) 注册与查询，避免在无协议栈阶段引入锁、队列和 DMA 生命周期复杂度。
3. **discovery-only 驱动也能形成管理闭环**: e1000/virtio_net 先注册 name/MAC/flags，netman 即可枚举设备，为后续 RX/TX ring、virtqueue 和协议栈提供稳定入口。

### 教训

1. **本地驱动 ABI 镜像要同步**: DKM 驱动当前各自内联 `dkm_kernel_api` 布局，新增 typed net API 时必须同步 e1000/virtio_net 的本地结构定义，否则字段访问容易退化成裸偏移错误。
2. **初始化器指定字段要按真实结构位置**: `block` 位于 `dkm_kernel_api` 尾部，不能因阅读局部字段时把 `.block` 放到 `.dma` 后导致重复初始化 warning。
3. **build.bat 不会自动重编外部 DKM 源**: 修改 `CODE/DKM/*/*.c` 后需确保对应 `.drv` 产物更新到 `SYSTEM/driver`，否则镜像可能仍打包旧驱动。

## 2026-07-01 — e1000 最小 RX/TX ring 与构建接入

### 经验

1. **先暴露能力位再接协议栈**: e1000 注册 netdev 时增加 `TX_READY/RX_READY` 标志，netman 可以明确区分“只发现设备”和“具备收发 ring”的设备。
2. **DMA ring 使用低 4G 连续页更稳**: 当前 DMA allocator 已提供 HHDM 虚拟地址与低 4G 物理地址，适合先搭 e1000 descriptor ring 和 2KB buffer 的最小闭环。
3. **网络设备应纳入主构建**: 把 e1000/virtio_net `.drv` 编译加入 build.ps1，避免手动编译驱动后忘记重新打包，导致 QEMU 仍加载旧驱动。

### 教训

1. **QEMU 测试脚本要提供真实 NIC**: 不加 `-device e1000e` 时 PCI 里没有 `8086:10d3`，e1000 驱动只能走 not found 路径，无法验证 RX/TX ring。
2. **网卡中断早期开启容易刷屏或阻塞**: 在 IOAPIC/MSI 路由完善前，e1000 中断保持 masked，收发先走 polling 路径更安全。
3. **串口过滤会掩盖启动卡点**: 管道过滤日志可避免 100MB 输出限制，但若启动阶段卡住，只看到少量匹配行；需要结合未过滤日志或关键阶段日志判断。

## 2026-07-02 — 修 DMA 与 e1000 buffer 卡死

### 经验

1. **DMA 分配本身已可用**: 通过逐页 `alloc_pages(1)` 加分段 raw_log 定位，确认 RX/TX descriptor ring 和每个 2KB buffer 页都能成功分配返回，卡点不在 allocator 的分配/清零逻辑。
2. **静态 BSS DMA pool 是临时方案**: 当前 `dma.c` 用内核 BSS 里的 16MiB `g_dma_static_pool` 加 `virt_to_phys()` 反推物理地址，作为 bump allocator 可分配，但网卡 bus-master 真实写入时的物理地址可靠性没有保证。
3. **稳定边界要固化**: e1000 现在在分配 descriptor ring 后直接返回 -6，只注册 `LINK_UP`，保证系统稳定启动到 netman/FirstInit，避免反复试错阻塞其他开发。

### 教训

1. **能靠日志“跑过”的 bug 往往是内存/时序问题**: e1000 buffer/ring enable 阶段“加 raw_log 能多跑几步、去掉就更早卡”，是典型的 DMA 目标内存被网卡写坏或时序竞争，不能靠加日志掩盖。
2. **bump allocator 不足以支撑真实 bus-master DMA**: 需要一个真正的物理页分配器（memmap USABLE → page bitmap，显式标记 kernel/module/framebuffer/arena/stack/DMA pool 已用），才能根治 e1000 RX/TX、AHCI PRDT、virtqueue 的连续物理内存需求。
3. **不要无限试错**: 反复缩小 ring 深度、搬动日志无法根治底层物理内存问题，应固化到已知稳定状态并明确记录卡点，把真正的物理页分配器作为下一步。

## 2026-07-02 — 物理页 bitmap 分配器根治 e1000 DMA

### 经验

1. **HHDM 直映射是 DMA 地址一致性的关键**: 把 `dma.c` 从"BSS 静态 pool + virt_to_phys 反推"改为"memmap USABLE → 物理页 bitmap"后，分配返回 `virt = hhdm_offset + phys`，保证 CPU 访问的虚拟地址与网卡 bus-master 使用的物理地址指向同一页，e1000 完整 RX/TX ring 启用不再卡死。
2. **依赖 Limine USABLE 语义可省掉手动避让**: Limine 的 USABLE 区域已排除 kernel/modules/framebuffer/bootloader 结构，直接用它建 bitmap 就不需要再手动 trim kernel/arena/stack，代码更简单也更不易错。
3. **验证要看能力位闭环**: netdev flags 从 `0x1`（仅 LINK_UP）变成 `0x7`（LINK_UP|TX_READY|RX_READY），netman 打印 `tx=ready rx=ready`，说明底层 DMA + ring enable 全链路打通。

### 教训

1. **反推物理地址不可靠**: 用内核高半虚拟地址（BSS）反推物理地址喂给设备 DMA，物理页与 CPU 映射不保证一致，是之前 e1000 一启用 RCTL/TCTL 就卡死的根因；DMA 缓冲必须来自明确的物理页分配。
2. **低 4G 约束要显式**: e1000 等 32 位 BAR 设备的 DMA 描述符/缓冲需落在低 4G，bitmap 分配器直接把上界钳到 `0x100000000` 并保留最低 1MiB，避免 legacy 区。

## 2026-07-02 — e1000 DHCP 联网闭环

### 经验

1. **PCI I/O 汇编必须 volatile**: `outl(0xCF8)` / `inl(0xCFC)` 访问 PCI config space 时必须使用 `asm volatile` 和 memory clobber，否则 `-O2` 下可能丢弃或重排，导致读取 BAR0 时仍拿到 vendor/device ID。
2. **验证 TX 要看 descriptor 完成位**: 只看到写 TDT 不代表网卡已发送；轮询 TX descriptor `DD` 位可以区分“API 调用成功”和“设备真正消费 descriptor”。
3. **DHCP 包要兼容 BOOTP 最小长度**: DHCP/BOOTP payload 补齐到 548 字节并计算 UDP checksum 后，QEMU user-net 能正常返回 OFFER/ACK。

### 教训

1. **BAR 读错会伪装成 DMA/RX 问题**: BAR0 被读成 `0x100e8086` 时，MMIO 实际指向错误地址，表现为 `STATUS=0`、TX descriptor 不完成，容易误判为 ring 或 DHCP 问题。
2. **早期网络调试要分层验证**: 先确认 PCI BAR → MMIO STATUS → TX DD → RX frame → DHCP 解析，避免在协议层反复修改掩盖底层设备访问错误。

## 2026-07-02 — FirstInit 无线网络选择界面

### 经验

1. **复用现有卡片/输入框最稳**: FirstInit 已有 `draw_rounded_input`、鼠标包解析和键盘轮询，直接替换旧网络选项页为 SSID 列表 + 密码框 + Connect 按钮，可以避免新增 UI 框架。
2. **配置先写入 user.conf 缓冲**: 当前缺 FAT32 write API，网络选择结果先进入加密配置缓冲，字段使用 `network.mode=wifi`、`network.ssid`、`network.password`、`network.ip=dhcp`、`network.dns=auto`，后续写盘可直接复用。

### 教训

1. **真实 Wi-Fi 扫描 API 尚未存在**: 当前界面只能展示模拟检测到的 SSID，后续需要网卡/无线驱动和扫描 API 后再替换数据源。
2. **FirstInit 仍不应接管系统联网**: UI 只收集用户选择和密码；实际联网仍由 netman/驱动负责，避免用户向导和系统初始化职责混在一起。

## 2026-07-02 — netman 完整早期联网闭环

### 经验

1. **联网闭环要分层验收**: DHCP 只证明拿到 IP，还需要 ARP 解析网关 MAC、DNS 查询外部域名，才能证明 TX/RX 与 QEMU user-net 实际可用。
2. **QEMU user-net 网关与 DNS 分离**: DHCP 返回网关 `10.0.2.2`，DNS 常为 `10.0.2.3`，应从 DHCP option 3/6 读取，缺失时再 fallback。
3. **保留正式阶段日志有助于验证**: `DHCP final rc`、`ARP final rc`、`DNS final rc`、`network final rc` 足以判断完整联网状态，不需要保留临时 debug 日志。

### 教训

1. **DNS parser 边界必须谨慎**: DNS response 使用压缩 name，解析 answer 时必须处理 `0xC0` 指针并检查 `rdlen`，否则容易越界。
2. **崩溃判断必须靠串口证据**: 用户观察到“崩溃重启”后，插桩证明确认联网阶段成功并进入 FirstInit；不能在没有阶段锚点证据时直接回滚功能。

## 2026-07-02 — 修复 FirstInit 网络页 SSID 指针数组崩溃

### 经验

1. **FirstInit PIE 中避免全局指针数组**: DSK 的 PIE loader 只处理有限重定位场景，UI 页面里的字符串列表应优先使用二维字符数组或栈上字符数组，避免 `const char *arr[]` 解引用崩溃。
2. **绘制函数要用阶段锚点定位**: 对网络页按 `redraw → row → fill → stroke → ssid → meta` 分层插桩，可以精确定位崩溃在第一条 SSID 文本绘制。

### 教训

1. **看似普通的字符串数组也是重定位风险**: `static const char *g_wifi_ssids[]` 在普通 C 程序中安全，但在当前 DSK 加载 FirstInit 的 PIE 环境下会变成运行期指针重定位风险。
2. **不要用跳过流程的调试改动进入最终版本**: 为了快速复现可以临时跳过账号/偏好页，但验证后必须恢复真实流程，避免引入与用户路径不一致的行为。

## 2026-07-03 — 用 Consolas TTF 替换 8x8 位图字体

### 经验

1. **裸金属 OS "用系统的 TTF" 的正确解读**: Deshab 没有 TTF 光栅化服务，"系统的 TTF" 应解读为构建时用宿主机 Windows 系统 TTF 字体（`C:\Windows\Fonts\consola.ttf`）预渲染为灰度位图，嵌入 FirstInit ELF，运行时仍走 alpha-blend 位图渲染路径。
2. **Consolas 18px 渲染参数**: advance=10px，'A' bbox=(0,3,10,14)，'g' bbox=(0,5,10,18)，11x18 格子完美适配（10px 字形 + 1px 间距，ASCII_STEP=11）。等宽字体适合 UI 输入框/菜单/按钮。
3. **统一灰度位图渲染路径**: 中文字体已用 `fb_bitmap_alpha()` 8bpp 灰度 alpha-blend，ASCII 字体改用同样的灰度位图 + alpha-blend 后，字形抗锯齿效果与中文一致，且 `fb_char()` 逻辑大幅简化（移除 8x8→10x16 缩放和边缘检测）。
4. **构建时预渲染脚本**: 新增 `CODE/font/render_ascii.py`，用 PIL 从 Consolas TTF 渲染 ASCII 32-126 共 95 个字符到 `CODE/firstInit/ascii_bitmaps.c`（18810 字节 glyph data），通过 `#include` 嵌入 main.c。

### 教训

1. **8x8 位图字体缩放到 10x16 形状怪异**: 手工 8x8 位图字体在小写字母上使用满高设计，2x2 近邻缩放后字形不自然；TTF 预渲染直接生成目标尺寸的灰度位图，避免缩放失真。
2. **字符范围要覆盖全部可打印 ASCII**: 原 `fb_char` 用 `ch >= ' ' && ch <= 'z'`（0x7a）截断，导致 `{` `|` `}` `~` 显示为空格；改为 `ch > '~'`（0x7e）后覆盖全部 95 个可打印字符。
3. **布局偏移要跟随字高调整**: ASCII_H 从 16 变为 18 后，硬编码的 `y+(h-16)/2` 需改为 `y+(h-ASCII_H)/2`，wifi 行的 `y+17`（选中标记居中）需改为 `y+16`（(50-18)/2=16）。

## 2026-08-03 — B7 DKM 侧 LAPIC + IOAPIC 接管中断路由（PIC→APIC 迁移第一步）

### 经验

1. **tick 链路自验是路由切换的最强证据**: 切换后用 PIT ch0 mode3 100Hz 作真实中断源，统计 handler 调用次数（阈值 >=2）。重复投递证明 LAPIC EOI 正确（无 EOI 时 edge/level 都只投递一次），比"读寄存器确认"可信得多。验证窗口用 TSC deadline 实现，不依赖任何中断。
2. **先保存再切换，失败完整降级**: 切换前保存 PIC IMR（0x21/0xA1），验证失败时 IOAPIC 全 mask + 恢复 IMR + 注销 handler + PIT 恢复 18.2Hz，系统无感回到 PIC 路由。stage0 时机早，此时所有 stage1-3 驱动尚未加载，降级零成本。
3. **FUCK 配置自解析 + 双开关**: `apic_route`（主开关，默认 0 保持 PIC）与 `apic_route_legacy`（legacy 线实验开关）分离，默认行为与旧版本完全一致，回归风险为零。
4. **tick vector 选 0xE0 避开了 LAPIC ISR 卡位陷阱**: LAPIC 规则要求新中断优先级 (vec>>4) 严格高于当前 ISR 才投递。若 tick 选 0x20-0x2F（优先级 2），一旦有无 EOI 的 0x2x 中断先卡位，tick 将永远静默，验证必失败。0xE0（优先级 14）无此问题。
5. **MADT ISO 必须查表，不能假设恒等**: QEMU q35 的 PIT IRQ0→GSI2（非恒等），键盘 IRQ1→GSI1（恒等）。`isa_irq_to_gsi` 先查 ISO 再回退恒等，两种拓扑都正确。
6. **验证脚本参数化**: w3c_route0/1.bat 固定 monitor 端口（45701/45702）+ TMP/TEMP 指向项目内目录（解决 edk2 pflash overlay 在中文用户名 Temp 下创建失败）+ `-snapshot` 保护镜像。

### 教训

1. **IOREDTBL 位布局抄错规范是隐蔽杀手**: 初版把 polarity/trigger/mask 错置于 bit 11/13/14（正确为 13/15/16）。mask 写入只读 remote IRR 位从未生效，"all masked" 名不副实；readback 读 remote IRR 恒 0，给出虚假证据。QEMU（全 active-high edge ISA）下功能侥幸正常，但真机 level/active-low PCI 线必炸。**硬件寄存器位定义必须对照原始 datasheet（82093AA），注释与代码同步修正，readback 证据必须与 claim 交叉验证。**
2. **build.ps1 漏编 apic.drv 导致修改不生效**: 原 `driver/platform/apic.drv` 是预置二进制，改 apic.c 后镜像不变。任何"驱动源码修改后行为无变化"都应先检查构建脚本是否真的编译了该驱动。
3. **QEMU pflash overlay 依赖 %TEMP%**: 中文用户名路径下临时 overlay 创建失败（`vl.XXXXXX: No such file or directory`），必须显式设置 TMP/TEMP 到纯 ASCII 路径。
4. **PIT mode3 计数器每 CLK 减 2**: 用 PIT 校准 TSC 时若按"减 1"假设，tsc_per_ms 偏小 2 倍，所有派生 delay 翻倍。校准代码必须考虑 mode2/mode3 差异（本轮 tick 计数 100@500ms 即为 2x 观察，功能不受影响但时序类代码需注意）。

## 2026-08-03 — B7 阶段2 UTSM 侧 LAPIC EOI 钩子落地 + legacy IRQ 持续投递 + tick 2x HPET 定论

### 经验

1. **EOI 钩子经 kernel_api 尾部追加是 ABI 安全的最小改动**: `register_apic_eoi` 追加在 `dkm_kernel_api` 末尾（不动既有字段偏移），UTSM 侧 `g_apic_eoi_hook` 为 NULL 时跳过调用，PIC 模式行为与旧版逐字节一致。新内核+旧驱动（钩子 NULL）与旧内核+新驱动（驱动判空跳过）双向兼容。
2. **handler 返回码区分"自 EOI"与"钩子兜底"**: idt_handler 两个 IRQ 分支（PIC 0x20-0x2F / APIC 0x30-0xFF）约定 handler 返回 1 = 已自行 LAPIC EOI（如阶段1 的 0xE0 tick handler），返回 0 = 由钩子补 EOI 安全网。分层职责让 handler 写法自由，链路永不漏 EOI。
3. **tick 验证 vector 从 0xE0 迁回 0x20 才能证明 legacy 分支**: 阶段1 用 0xE0 自 EOI 只证明了 APIC 分支（0x30+）；阶段2 把 tick handler 迁到 0x20（PIC 分支），与键盘 0x21/鼠标 0x2C 同一代码路径，500ms 内重复投递 101 次即证明 "legacy vector 分支 + 钩子 EOI" 全链路可持续。
4. **spurious 向量 0xFF 必须特判跳过 EOI 钩子**: LAPIC spurious（SVR 低 8 位）规范上不置 ISR 位、永不需 EOI；若对 0xFF 也调钩子写 EOI，嵌套场景下会误清真正 in-service 的最高优先级位。
5. **HPET 主计数器是 QEMU 下最干净的绝对时基**: 解析 ACPI HPET 表 → MMIO 主计数器（100MHz，周期从 GCAP_ID[63:32] 读飞秒值）→ 直接换算 us，不依赖 PIT/TSC 任何先验。tick 2x 的两个候选假设（TSC 校准测半 vs PIT mode3 双触发）用一次对拍即定案：calib 窗口 10132us 准确 + verify 窗口 504883us 准确 + 实测 200Hz → PIT mode3 方波全周期双沿触发定论（详见 BUG-20260803-009）。
6. **观察窗长度必须覆盖注入链路延迟**: "内核打锚点 → 串口落盘（块缓冲）→ 测试脚本轮询可见 → monitor 注入" 链路延迟 ~1s 量级，3s 窗口实测被吃光（注入到达时窗口已关闭，计数=0），6000ms 才稳定拿到注入数据（鼠标 27 包 / 键盘 8 包）。

### 教训

1. **PS/2 控制器交互期间绝不能有 IRQ handler 偷读 0x60**: diag handler 先于 `ps2_aux_enable_stream()` 注册时，控制器命令响应字节（0xFA ACK 等）被 handler 当数据包读走，轮询侧 wait_output 超时（表现为 F3 nack 假象，实为字节被偷）。铁律：控制器命令序列全轮询完成后再注册 handler；交互期间到达的 IRQ 因 handler 未注册只会被 EOI 丢弃，无副作用。
2. **真机级别的寄存器语义要按设备类强制**: PCI INTx（e1000 IRQ11）无 MADT ISO 覆盖时必须强制 level/active-low（PCI 规范 + QEMU pci irq 电平语义 + Linux/Windows 同配置），ISA 线才按 ISO 表缺省 edge/high；一概套 ISO 缺省会让 PCI 设备中断一次性投递后卡死。
3. **验证脚本数组参数经 -File 传递会被合并**: `powershell -File xxx.ps1 -Scenarios @('B1','B2')` 中数组变单字符串 "B1 B2"（场景被跳过且报"未知场景"），需用 `powershell -Command "& 'xxx.ps1' -Scenarios 'B1','B2'"`。


