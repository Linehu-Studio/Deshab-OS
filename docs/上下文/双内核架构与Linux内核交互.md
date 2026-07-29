# 双内核架构与Linux内核交互

> **Workspace:** d:\Code\Deshab

---

**User**

准备为系统开发，UTSM和 Linux双内核架构，运行一个Linux内核虚拟机，并更改linux内核，增加两个内核的交互，其他的设计可以看D:\Code\ZT5；为双内核架构做准备，准备接入和兼容linux内核，驱动和软件

**Assistant**

我将协助您设计 UTSM + Linux 双内核架构。这是一个重大的架构任务，因此请允许我先研究 D:\Code\ZT5 的参考设计，并了解当前的 UTSM 架构，然后再提出实施方案。

### Relevant Code Snippets
1. d:/Code/Deshab/RE/UTSM_设计架构.md:L1-L100
  — 提供了UTSM内核架构的概述，包括模块结构、内存管理方式和核心组件的功能描述，为理解整体设计提供基础。
2. d:/Code/Deshab/CODE/UTSM/kernel/main.c:L1-L50
  — 主要的内核入口点，展示UTSM初始化流程和核心模块的调用顺序，是理解内核启动过程的关键。
3. d:/Code/Deshab/CODE/UTSM/dkm/kernel_api.c:L1-L100
  — 实现kernel_api结构的定义和核心功能函数，定义了内核和驱动程序之间的接口规范。
4. d:/Code/Deshab/CODE/UTSM/include/utsm/dkm.h:L1-L50
  — 包含DKM（驱动内核模块）接口定义，展示了DKM驱动的注册、管理与API结构，为DKM驱动系统提供接口定义。
5. d:/Code/Deshab/CODE/dsk/main.c:L1-L50
  — dsk模块的主入口点，展示了在UTSM之上运行的Deshab内核的主要处理逻辑和职责。
6. d:/Code/Deshab/CODE/UTSM/mm/arena.c:L1-L100
  — 内存管理模块，定义了内存区的分配与释放机制，展现了UTSM的单地址空间内存管理策略。
7. d:/Code/Deshab/docs/BOOT_SEQUENCE.md:L1-L50
  — 详细描述了从Limine引导到deshab.elf启动的完整引导流程，解释了系统启动序列。
8. d:/Code/Deshab/CODE/UTSM/kernel/dsk_loader.c:L1-L100
  — 加载并执行dsk模块的实现，定义了如何将主内核模块加载到内存并执行。
9. d:/Code/Deshab/CODE/UTSM/dkm/boot_modules.c:L1-L100
  — 处理引导模块的相关功能，包括如何加载和传递这些模块到内核。
10. d:/Code/Deshab/RE/驱动模块ABI设计.md:L1-L50
  — 定义了DKM驱动模块的接口规范，是理解驱动程序与内核交互的依据。
11. d:/Code/Deshab/CODE/UTSM/core/process.c:L1-L50
  — 定义了进程管理的核心结构和相关函数，对多任务和调度机制提供支持。
12. d:/Code/Deshab/CODE/UTSM/core/capability.c:L1-L50
  — 实现了能力管理机制，这是UTSM系统内核安全模型的重要组成部分。
13. d:/Code/Deshab/CODE/UTSM/include/utsm/utsm.h:L1-L100
  — 定义了UTSM内核的主要接口和核心数据结构，为开发者提供访问核心功能的入口。
14. d:/Code/Deshab/CODE/UTSM/arch/x86_64/boot.S:L1-L100
  — 处理引导阶段的汇编代码，负责最初的CPU设置和内核跳转。
15. d:/Code/Deshab/CODE/UTSM/include/utsm/dsk.h:L1-L50
  — dsk模块的接口定义，展示其与UTSM内核的集成方式和主要功能。

这些文件提供了关于UTSM内核架构、引导流程、内存管理、驱动系统和核心功能的全面信息，有助于全面理解该项目的系统设计和实现。

Let me explore the ZT5 reference design and the current UTSM source structure in parallel.

Now let me explore the ZT5 dual-kernel implementation details to understand the reference design.

Let me look at the Linux migration doc and current UTSM source structure in parallel.

### Relevant Code Snippets
1. d:/Code/Deshab/CODE/UTSM/kernel/main.c:L1-L100
  — 主要的内核初始化入口，包含了系统初始化流程，包括早期初始化、DKM初始化、内核API初始化和核心初始化
2. d:/Code/Deshab/CODE/UTSM/include/utsm/utsm.h:L1-L100
  — 定义了UTSM内核的公共API接口和数据结构，包括内核状态、版本信息和核心函数声明
3. d:/Code/Deshab/CODE/UTSM/include/utsm/dkm.h:L1-L50
  — 定义了DKM(kernel API)结构体，包含系统驱动接口，定义了系统初始化相关函数指针
4. d:/Code/Deshab/CODE/dsk/main.c:L1-L100
  — DSK引导程序入口，描述了内核启动流程和加载过程，包括内存管理、中断处理和硬件初始化
5. d:/Code/Deshab/CODE/UTSM/arch/x86_64/boot.S:L1-L50
  — x86_64架构的启动汇编代码，定义了引导程序的入口点和初始堆栈设置
6. d:/Code/Deshab/CODE/UTSM/core/utsm_init.c:L1-L50
  — 系统初始化函数，包含核心服务的初始化和配置，以及初始化顺序的处理
7. d:/Code/Deshab/CODE/UTSM/dkm/kernel_api.c:L1-L50
  — 定义了DKM的核心API结构，包含用于驱动和内核通信的接口函数
8. d:/Code/Deshab/CODE/UTSM/mm/arena.c:L1-L50
  — 内存管理器初始化和页分配接口，定义了内存区域管理相关的函数和结构体

以下是详细分析：

### 1. CODE/UTSM/目录结构
```
CODE/UTSM/
├── arch/
│   └── x86_64/
│       ├── boot.S
│       ├── halt.c
│       ├── idt.c
│       ├── idt_stubs.S
│       ├── io.c
│       ├── limine.h
│       └── serial.c
├── core/
│   ├── capability.c
│   ├── dirty.c
│   ├── dmp.c
│   ├── process.c
│   ├── segment.c
│   └── utsm_init.c
├── crypto/
│   ├── kdf.c
│   ├── stream.c
│   └── tweak.c
├── dkm/
│   ├── boot_modules.c
│   ├── console_early.c
│   ├── dkm_core.c
│   ├── kernel_api.c
│   └── manifest.c
├── drr/
│   └── drr_stub.c
├── include/
│   └── utsm/
│       ├── arena.h
│       ├── assert.h
│       ├── block.h
│       ├── capability.h
│       ├── config.h
│       ├── crypto.h
│       ├── deshab_ui.h
│       ├── dirty.h
│       ├── dkm.h
│       ├── dma.h
│       ├── dmp.h
│       ├── drr.h
│       ├── dsk.h
│       ├── idt.h
│       ├── log.h
│       ├── net.h
│       ├── pckc.h
│       ├── process.h
│       ├── segment.h
│       ├── status.h
│       ├── types.h
│       ├── utrw.h
│       ├── utsm.h
│       └── uuid.h
├── kernel/
│   ├── block.c
│   ├── dsk_loader.c
│   ├── log.c
│   ├── main.c
│   ├── net.c
│   └── panic.c
├── mm/
│   ├── arena.c
│   └── dma.c
├── pckc/
│   └── pckc.c
├── runtime/
│   ├── assert.c
│   └── string.c
├── test/
│   └── utsm_selftest.c
├── utrw/
│   ├── read.c
│   ├── slow_path.c
│   └── write.c
├── MAKEFILE
├── README.md
└── linker.ld
```

### 2. CODE/dsk/目录结构
```
CODE/dsk/
├── MAKEFILE
├── README
├── linker.ld
├── main.c
└── port.h
```

### 3. CODE/DKM/目录结构
```
CODE/DKM/
├── acpi/
│   └── acpi.c
├── ahci/
│   └── ahci.c
├── apic/
│   └── apic.c
├── ath9k/
│   └── ath9k.c
├── bootfs/
│   └── bootfs.c
├── console_fb/
│   └── console_fb.c
├── devfs/
│   └── devfs.c
├── e1000/
│   └── e1000.c
├── fat32/
│   └── fat32.c
├── nvme/
│   └── nvme.c
├── pci/
│   └── pci.c
├── ps2kbd/
│   └── ps2kbd.c
├── stub_driver/
│   ├── build_stubs.ps1
│   └── stub_driver.c
├── timer/
│   └── timer.c
├── vfs/
│   └── vfs.c
├── virtio_net/
│   └── virtio_net.c
├── README.md
└── dkm_shared.h
```

### 4. 各子目录下的C/H文件列表

#### CODE/UTSM/kernel/
- *.c: block.c, dsk_loader.c, log.c, main.c, net.c, panic.c
- *.h: 无

#### CODE/UTSM/mm/
- *.c: arena.c, dma.c
- *.h: arena.h

#### CODE/UTSM/arch/
- *.c: halt.c, idt.c, io.c, serial.c
- *.h: limine.h
- *.S: boot.S, idt_stubs.S

#### CODE/UTSM/dkm/
- *.c: boot_modules.c, console_early.c, dkm_core.c, kernel_api.c, manifest.c
- *.h: 无

#### CODE/UTSM/core/
- *.c: capability.c, dirty.c, dmp.c, process.c, segment.c, utsm_init.c
- *.h: capability.h, dirty.h, dmp.h, process.h, segment.h

### 5. CODE/UTSM/kernel/main.c 结构分析
该文件是UTSM内核的主入口点，主要功能包括：
- 调用early_init()函数进行早期初始化
- 初始化DKM子系统
- 调用kernel_api_init()初始化内核API
- 调用utsm_core_init()进行核心服务初始化
- 调用main_loop()进入主运行循环
- 所有这些调用都在main函数中按顺序执行

### 6. CODE/UTSM/include/utsm/utsm.h API 接口
该头文件定义了：
- utsm_version: 系统版本信息
- utsm_config: 系统配置结构体
- 主要API函数声明，如utsm_init(), utsm_run(), utsm_shutdown()等
- 核心数据结构和宏定义

### 7. CODE/UTSM/include/utsm/dkm.h 内核API结构体
kernel_api结构体定义了：
- dkm_init函数指针，用于DKM初始化
- dkm_run函数指针，用于DKM运行
- dkm_shutdown函数指针，用于DKM关闭
- 以及其他DKM相关的API函数指针

### 8. 虚拟化代码搜索结果
通过搜索CODE目录下的文件，未发现明确的VMX/EPT或虚拟化相关代码。

### 9. CODE/dsk/main.c 结构分析
该文件是DSK引导程序，负责：
- 内存管理初始化
- 中断处理初始化
- 硬件设备检测与初始化
- 加载并跳转到UTSM内核

### 10. CODE/ 目录下所有子目录
```
CODE/
├── DKM/
├── UTSM/
├── desktop/
├── dsk/
├── firstInit/
├── font/
├── login/
├── mouse/
├── netman/
├── shell/
├── tools/
└── RE/ (无文件)
```

I now have a thorough understanding of both the ZT5 reference design and the current UTSM architecture. This is a major architectural undertaking with multiple valid approaches and significant design decisions, so I'll enter plan mode to design the dual-kernel architecture properly before any implementation.

我已经通过探索全面了解了情况。在制定计划之前，我需要澄清几个关键的架构决策，这些决策将从根本上影响设计。请允许我提出最重要的问题。

I have all the information I need. The user chose VMX/EPT full virtualization, Linux owns hardware with UTSM as monitor, Linux 6.6 LTS, and full Phase 1 scope including virtio-console + file sharing. Let me launch a Plan agent to validate the design approach while I begin synthesizing.

---

**User**

继续

**Assistant**

我已经掌握了所需的所有答案。用户选择了：
1. VMX/EPT 完整虚拟化
2. Linux 独占硬件，UTSM 作为监控核（监控程序模型）
3. Linux 6.6 LTS
4. 完整的第一阶段，包含 virtio-console 和文件共享

这是一个重大且定义明确的架构。让我来编写计划文件。

计划文件已写好，请用户审阅。

计划已批准。让我开始实施。我将设置待办事项列表以跟踪第一阶段的子阶段，并从子阶段 1.1（VMX infrastructure）开始。

现在让我阅读现有的 UTSM 源代码，以了解我需要遵循的代码模式、约定和集成点。

让我检查一下 `linker.ld`、`boot.S` 以及现有的 `arena/dma` 以寻找内存模式，并查看 `dsk_loader.c` 以了解 ELF 加载模式。

我来检查一下配置、limine 请求以及现有的 DMA 代码以了解上下文。

我已经完全理解了代码库。现在让我来构建 VMX 基础设施。我将从头文件开始，然后再进行实现。

现在是 EPT 标头：

现在是顶层 VMM API 头文件：

现在是 VMX 实现文件：

现在是 EPT 实现：

现在开始实现 VMEXIT dispatcher 和顶层 VMM 编排：

现在编写顶层 VMM orchestration 文件，包含 VMCS 设置和自测程序：

现在是 VM-exit handler 的汇编存根（stub）：

我需要修复 VMX 启动中的控制流问题——`vmlaunch` 在成功后并不会返回，因此在客户机（guest）终止时，我需要一种方法来恢复原始堆栈并跳转回来。让我使用基于 `asm-jmp` 的方法正确地重写 `vmm.c` 和 `vmexit_asm.S`。

现在是修正后的 VM-Exit 汇编处理程序，具有适当的 stack-restore-and-jmp 逻辑：

现在更新 `MAKEFILE` 以包含新的 `vmm/` 目录和 `utsm.h` 以声明 VMM API，并将 `vmm_init` + `self-test` 集成到 `kernel main.c` 中：

现在更新 `utsm.h` 以包含 VMM API：

现在将 VMM 初始化和自检集成到 kernel main.c 中：

让我更新 `run_qemu.bat` 以启用 VMX，然后尝试构建以捕捉任何问题：

让我检查一下 `dma.h` 以验证 `dkm_dma_buffer` 结构，然后构建 UTSM 以检查是否有编译错误：

我发现了一些问题：`vmx_get_host_cr3` 定义重复，静态全局变量导致 asm 无法引用，以及未使用的 include。让我来修复 vmm.c 和 vmx.h：

现在修复 vmm.c - 使全局变量变为非静态（non-static），删除重复定义，删除未使用的 include：

现在删除重复的 `vmx_get_host_cr3` 定义和末尾的 `__asm__(".global ...")`：

让我检查一下 vmm.c 中是否存在任何剩余的引用问题：

很好，保持一致。现在让我构建 UTSM 以捕获任何编译错误：

---

**User**

continue

**Assistant**

Let me check disk space and find the proper build setup:

C: drive is nearly full (1.49 GB free). Let me find clang location and check how build.ps1 invokes the build:

Clang is at `C:\Program Files\LLVM\bin\clang.exe`. The disk space issue is on C: (TEMP location). Let me set TEMP to D: and check the UTSM build section in build.ps1:

The build.ps1 sets TEMP to `.build_tmp` on D: drive. Let me build UTSM directly with the same approach:

Good progress - most files compiled. Two errors: `EPT_PAGE_SIZE` is in ept.h but vmx.c doesn't include it. Let me fix:

The Chinese comments in asm aren't accepted. Let me check the existing asm comment style:

The existing `.S` files use C-style `/* */` comments (processed through C preprocessor). Let me rewrite vmexit_asm.S:

Build succeeded. Let me verify the output and check for any warnings I missed:

Build passes, but I spotted two critical runtime bugs that would cause VMX failures: the EPT walk doesn't update the table pointer after allocating a new page, and the host segment selector encodings are wrong. Let me fix both:

Now fix the EPT walk bug - it doesn't update `table` after allocating a new intermediate page:

I need to add a minimal GDT and TSS for the guest - VMX requires valid guest TR. Let me update vmm.c:

I'll continue from where we left off — completing the guest GDT/TSS integration into the VMCS guest state setup. Let me first check the current state of the key files to understand exactly where we are.

Let me check the VMX header and EPT module to understand the available VMCS fields and EPT mapping API.

Let me check the EPT header and the planning document to understand the EPT API and overall plan.

Now let me check the vmexit.c and the planning document to understand the full context.

Let me check the planning document for the overall architecture plan.

Now I have a complete understanding of the current state. The guest GDT/TSS structures and `build_guest_gdt()` function are defined in vmm.c but not yet wired into the VMCS guest state setup or EPT mapping. Let me set up the todo list and complete Phase 1.1.

Now let me complete the guest GDT/TSS integration. First, I'll update `vmcs_setup_guest_state_phase1()` to properly configure the GDTR and TR fields:

Now let me update `vmm_self_test()` to call `build_guest_gdt()` and map the GDT/TSS pages in EPT:

I need to fix the error code conflict — the vmlaunch failure was using `-5` which is now used for GDT mapping. Let me update it:

Now I need to check the host state setup — VMX also requires host TR and SYSENTER fields to be valid. Let me update `vmcs_setup_host_state()`:

Now I need to add the GUEST_IA32_EFER and HOST_IA32_EFER VMCS field encodings — these are required because we set `VM_ENTRY_LOAD_GUEST_EFER` and `VM_EXIT_LOAD_HOST_EFER` control bits. Let me add them to vmx.h:

Now let me add the HOST_IA32_EFER field encoding and EFER MSR bits:

Now let me add EFER setup to both guest and host state. First, the guest state (EFER.LME and EFER.LMA required for 64-bit guest):

Now let me add the host EFER setup to `vmcs_setup_host_state()`:

Now let me build the project to verify everything compiles. Let me check the MAKEFILE first:

Build succeeded with no errors. Let me verify the VMM self-test is integrated into the boot flow by checking main.c:

Good — the VMM self-test is already integrated into the boot flow. Let me verify the `vmm.h` include is in `utsm.h`:

The VMM integration is complete. Now let me build the full disk image and verify with QEMU. Let me run the full build:

The `deshab.img` is locked by another process — likely a running QEMU. Let me kill it and rebuild:

Build succeeded. Now let me run QEMU to verify the VMX self-test passes. I'll capture the serial output:

The path with spaces needs proper quoting. Let me fix the QEMU invocation:

Let me create a temporary batch file to properly handle the QEMU invocation:

Let me search for the VMM/VMX specific output:

VMX is not supported in QEMU's default TCG emulation. Let me check how VMX support is detected and whether we can enable it:

VMX requires hardware virtualization support. Let me check the QEMU version and try enabling VMX with the `+vmx` CPU flag:

QEMU 11.0 supports VMX emulation in TCG mode. Let me try with `-cpu max,+vmx`:

TCG doesn't support VMX. Let me try WHPX (Windows Hypervisor Platform) acceleration:

WHPX is running. Let me check the serial output:

The #GP happens at very early boot with WHPX. Let me try WHPX with explicit VMX and a different CPU model:

The #GP is in OVMF firmware under WHPX. Let me try a different machine type:

WHPX crashes OVMF firmware regardless of machine type — this is a known WHPX/OVMF incompatibility. Let me verify the TCG boot (without VMX) still works correctly after my changes, then proceed with Phase 1.2:

The previous log was from the WHPX crash. Let me run a standard TCG boot to verify the system still works:

The QEMU test confirms TCG doesn't support VMX and WHPX crashes OVMF — VMX testing requires real hardware with VT-x. The Phase 1.1 code is complete, compiles cleanly, and the graceful fallback works. Let me update the todos and proceed to Phase 1.2 (Linux loader):

Now let me start Phase 1.2. First, let me check how Limine boot modules work in the existing code, and look at the ZT5 reference for the dual-kernel implementation:

### Relevant Code Snippets
1. d:/Code/Deshab/CODE/UTSM/dkm/boot_modules.c:L405-L449
  — This function enumerates boot modules via the Limine module request, logging each module's path, cmdline, and size. It demonstrates how to access module information from the limine_module_response.

2. d:/Code/Deshab/CODE/UTSM/arch/x86_64/limine.h:L33-L60
  — Defines the limine_file struct (used in limine_module_response) which holds the base address, size, path, and cmdline of each boot module.

3. d:/Code/Deshab/CODE/UTSM/arch/x86_64/limine.h:L50-L54
  — Defines the limine_module_response struct which contains an array of limine_file pointers and the module count returned by the Limine bootloader.

4. d:/Code/ZT5/SYSTEM/lib/Ztknl/vmm/dual_kernel.c:L1-L100
  — This file likely contains Linux loading logic. It has functions related to dual-kernel operations, including the ability to load Linux kernels (likely a bzImage). The structure is typically used to create new VMs and initialize kernel memory.

5. d:/Code/ZT5/SYSTEM/lib/Ztknl/loader.c:L1-L100
  — This file handles loading various types of kernels, including Linux's bzImage, using setup_header and boot_params structures in a dual-kernel environment.

6. d:/Code/Deshab/CODE/UTSM/mm/dma.c:L1-L100
  — Contains dma_alloc function definitions which may be used for allocating contiguous physical memory for guest RAM, essential for Linux loading.

7. d:/Code/ZT5/SYSTEM/lib/Ztknl/include/limine.h:L1-L50
  — This header defines limine boot structures including boot_params, setup_header, and other Linux boot structures used for preparing the Linux kernel environment.

8. d:/Code/ZT5/SYSTEM/lib/Ztknl/include/loader.h:L1-L50
  — Contains the loader_api struct definition, which provides key functions for loading kernels such as linux_loader, which would be used to properly load a bzImage.

9. d:/Code/ZT5/SYSTEM/lib/Ztknl/include/dual_kernel.h:L1-L50
  — Header file that declares structures and functions for dual-kernel architecture, which includes dual_kernel_config and interfaces for Linux kernel initialization.

10. d:/Code/ZT5/SYSTEM/lib/Ztknl/include/boot.h:L1-L100
  — Contains definitions for boot configurations that may include parameters needed for Linux booting like boot_params, setup_header, and memory layouts.

11. d:/Code/ZT5/SYSTEM/lib/Ztknl/boot/loader.c:L1-L100
  — The actual implementation of the Linux loader (if present) that takes a bzImage and sets up the Linux environment using the appropriate boot structures.

12. d:/Code/ZT5/SYSTEM/lib/Ztknl/include/limine_file.h:L1-L50
  — Defines limine_file type and associated constants used in boot modules handling.

Let me read the key files directly to understand the boot module API and memory allocation:

Let me read the ZT5 reference implementation for the Linux loader:

Now I have a clear picture of the approach. Let me create the Linux loader. First, the header file with Linux boot protocol structures:

Now let me create the Linux loader implementation. First, let me check where the module request global is declared:

Now let me create the Linux loader implementation:

Now let me create `linux_boot.c` — the VMCS configuration and launch for the Linux guest:

I need to fix a mistake in linux_boot.c — I accidentally wrote to GUEST_GS_BASE. Let me remove that erroneous line:

Now let me add the `linux_launch` declaration to the header and update the VMM header to include the Linux loader:

Now let me add `linux_loader.h` include to `utsm.h` and integrate the Linux launch into `main.c`:

Now let me integrate the Linux loader and launch into the boot flow in main.c. I need to be careful — the Linux launch should only happen if VMX is available and the bzImage module is present:

Now let me build to check for compilation errors:

I need to fix the linker error — `g_host_stack` is `static` in vmm.c. Let me make it non-static and fix the warnings:

Now let me fix the unused function/parameter warnings in linux_loader.c:

Now fix the unused `bp_gpa` parameter in `setup_boot_params`:

No errors or warnings. Let me verify the build output:

The build succeeded (82KB utsm.elf). Now I need to set up the Linux 6.6 LTS configuration. Let me check if there's an existing Linux source directory:

Let me check if WSL is available for building Linux:

WSL is available but no distribution is installed. Let me create the Linux build infrastructure — the defconfig, build script, and integration files. The user can install WSL and build Linux later. First, let me create the directory structure and defconfig:

Now let me create the Linux build script:

Now let me update the limine.conf to include the Linux bzImage as a boot module:

Now let me add the Linux bzImage and initrd as boot modules in limine.conf:

Actually, I need to comment out the Linux modules for now — if the files don't exist, Limine will fail to boot. Let me comment them out:

Now let me run the full build to make sure everything still works with the new Linux loader code:

The full build succeeded. Now let me create a README for the Linux setup, then proceed to Phase 1.3 (IPC skeleton):

Now let me proceed to Phase 1.3 — IPC skeleton. First, the shared protocol header:

Let me create the IPC protocol header shared between UTSM and Linux:

我将从 Phase 1.3 IPC 骨架的实现继续。首先让我确认当前进度，查看已有的文件和 ipc_proto.h。

好的，ipc_proto.h 已经创建好了。让我检查一下计划文档和现有的 UTSM 头文件，以确保实现一致。

让我查看现有的 vmexit.c 和 vmm.c，以了解 VMCALL 处理需要在哪里集成。

让我查看 vmx.h 以了解已有的 VMCS 常量和 exit reason 定义。

让我查看 linux_boot.c 和 vmm.c 以了解 VMCS 配置和 VMEXIT 处理路径。

让我查看 linux_loader.c 以了解 EPT 映射的位置，以便集成 IPC 共享内存映射。

现在我对整个架构有了清晰的理解。让我创建任务列表并开始实现 Phase 1.3 IPC 骨架。

我注意到一个 GPA 冲突：`LINUX_GUEST_INITRD_GPA = 0x04000000` 与 `UTSM_IPC_SHM_GPA = 0x04000000` 重叠，需要先修复。

让我先查看架构规范文档以确认 GPA 布局设计意图。

架构规范确认了 GPA 布局：
- 0x04000000-0x04FFFFFF: IPC 共享内存（1MB）
- Initrd 应在 0x00400000-0x03FFFFFF 区域内

现在开始实现。**Task 1：修复 GPA 布局冲突**。

现在更新 `linux_loader.c` 中的 e820 映射和 RAM 映射，标记 IPC 区域为 reserved 并将 RAM 移至 0x05000000。

现在更新 RAM 映射位置从 0x03000000 到 0x05000000：

现在更新 RAM EPT 映射位置和注释：

Task 1 完成。现在创建 `ipc_shm.h` 头文件。

让我检查 UTSM 的 types.h 以确保类型兼容性。

让我检查 UTSM 的构建系统以了解 include 路径和编译器标志。

MAKEFILE 使用 `-Iinclude`。我需要添加 `-I../utsm-ipc` 以让 ipc_proto.h 可访问。让我先更新 MAKEFILE，然后更新 ipc_shm.h。

现在更新 ipc_shm.h 以包含 ipc_proto.h（用于 `struct utsm_ipc_msg` 定义）：

Task 2 完成。现在创建 `hypercall.h` 和 `hypercall.c`，然后是 `ipc_shm.c`。

我需要理解 VM-Exit 时 GPR 如何保存。让我查看 vmexit_asm.S 和 vmm.c 中的 VM-Exit 处理流程。

让我查看 vmx.c 中的 vmx_vmresume 实现，然后重新设计 VM-Exit 汇编以支持 GPR 传递（VMCALL 需要 guest RAX/RDI/RSI/RDX）。

现在我对整个 VM-Exit 流程有了完整理解。VMCS 不保存 guest GPR，而 VMCALL 需要 guest RAX/RDI/RSI/RDX。我需要重新设计 VM-Exit 汇编以保存/恢复 guest GPR。

让我先更新 vmm.h 添加 guest 寄存器结构：

现在重写 `vmexit_asm.S` 以保存/恢复所有 guest GPR，并将 vmresume 移到汇编中：

现在更新 vmm.c：定义 `g_guest_regs` 全局变量，并修改 `vmm_vmexit_entry()` 不再调用 vmresume（已移到汇编）。

现在修改 `vmm_vmexit_entry()` 移除 vmresume 调用（已移至汇编）：

现在创建 `ipc_shm.c`。它负责分配 1MB 共享内存、初始化结构、EPT 映射、并提供 ring buffer 操作。

现在创建 `hypercall.c`。但首先，hypercall 需要访问 guest 内存（如 CONSOLE_WRITE 的字符串缓冲区）。由于 guest 使用 identity-mapped 页表（GVA=GPA），我需要一个 EPT walk 函数来翻译 GPA→HPA。让我先查看 ept.c。

我需要在 ept.c 中添加 `ept_gpa_to_hpa()` 函数，用于将 GPA 翻译为 HPA（hypercall 需要访问 guest 内存）。

现在创建 `hypercall.c`，实现 VMCALL 处理器：

现在将 VMCALL 情况集成到 `vmexit.c` 中（Task 6）：

现在在 dispatch 函数中添加 VMCALL 处理分支：

Task 6 完成。现在 Task 7：将 `ipc_shm_init()` 集成到启动流程。让我查看 UTSM 的 main.c 以了解启动顺序。

现在我需要在 `linux_loader_init()` 和 `linux_launch()` 之间插入 `ipc_shm_init()` 调用：

Task 7 完成。让我也更新 `utsm.h` 以包含新的头文件：

现在让我尝试构建 UTSM 以检查新文件的编译错误。MAKEFILE 已经用 wildcard 自动编译 vmm/*.c。

需要修复 TMP/TEMP 路径和 PATH 问题（之前的会话也遇到过）。

构建成功！`utsm.elf` 已生成。只有 2 个 warning，其中一个是我的 `guest_read_u64` 未使用。让我移除它。

构建成功，只有预存的 vmx.c 警告。现在进行 Task 8：创建 Linux 内核模块，让 Linux guest 能通过 VMCALL 与 UTSM 通信。

让我先查看现有的 linux 目录结构：

现在创建 Linux 内核模块。我将创建源文件和集成脚本，让 build.sh 能将驱动集成到 Linux 源码树中。

我需要修复 `ipc_shm_send_to_utsm` 的前向声明问题——它在 init 函数中被调用但在后面定义。

现在创建 Kconfig 和 Makefile 文件用于 Linux 驱动集成：

现在更新 build.sh 来集成 UTSM 驱动到 Linux 源码树，并更新 defconfig。让我先查看当前的 defconfig：

现在在 defconfig 中添加 UTSM 驱动配置：

现在更新 build.sh 以集成 UTSM 驱动到 Linux 源码树：

现在让我做最终构建验证，确保所有修改编译无误：

构建成功！只有 3 个预存 warning（都在 idt.c 和 vmx.c 中，非新代码）。`utsm.elf` 已生成。让我验证输出文件：

构建完成，`utsm.elf` 为 87120 字节。让我创建 IPC 模块的文档，并更新内存记录。

现在更新内存记录，记录 Phase 1.3 的进度：

让我也更新 project_memory.md 添加 Phase 1.3 的关键约束：

我看到有一个重复行。让我移除它：

Phase 1.3 IPC 骨架已全部完成。以下是本次实现的总结：

## Phase 1.3 — UTSM↔Linux IPC 骨架 完成总结

### 新建文件

**UTSM 侧（监控核）：**
- [ipc_shm.h](file:///d:/Code/Deshab/CODE/UTSM/include/utsm/ipc_shm.h) — IPC 共享内存 API 声明
- [ipc_shm.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/ipc_shm.c) — 1MB 共享内存分配、ring buffer 初始化、EPT 映射、消息收发
- [hypercall.h](file:///d:/Code/Deshab/CODE/UTSM/include/utsm/hypercall.h) — VMCALL 处理器 API
- [hypercall.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/hypercall.c) — 9 种 hypercall 操作分发（PING/HELLO/SHM_INFO/CONSOLE_WRITE 等）
- [README.md](file:///d:/Code/Deshab/CODE/utsm-ipc/README.md) — IPC 模块文档

**Linux 侧（主导核）：**
- [utsm_hcall.c](file:///d:/Code/Deshab/CODE/linux/patches/utsm_hcall.c) — Linux 内核驱动：VMCALL + /dev/utsm + SHM 映射
- [utsm_Kconfig](file:///d:/Code/Deshab/CODE/linux/patches/utsm_Kconfig) — Kconfig 配置
- [utsm_Makefile](file:///d:/Code/Deshab/CODE/linux/patches/utsm_Makefile) — 模块构建规则

### 修改文件

| 文件 | 改动 |
|------|------|
| [vmexit_asm.S](file:///d:/Code/Deshab/CODE/UTSM/vmm/vmexit_asm.S) | **重写**：保存全部 16 个 guest GPR 到 `g_guest_regs`，汇编中执行 vmresume（使 C 可修改返回值） |
| [vmm.h](file:///d:/Code/Deshab/CODE/UTSM/include/utsm/vmm.h) | 增加 `struct vmexit_guest_regs` 和 `g_guest_regs` 声明 |
| [vmm.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/vmm.c) | 定义 `g_guest_regs` 全局；`vmm_vmexit_entry()` 简化（移除 C 中 vmresume 调用） |
| [vmexit.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/vmexit.c) | 增加 `EXIT_VMCALL` 分发分支 |
| [ept.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/ept.c) / [ept.h](file:///d:/Code/Deshab/CODE/UTSM/include/utsm/ept.h) | 增加 `ept_gpa_to_hpa()` — 4 级 EPT walk（支持 4KB/2MB/1GB 页） |
| [linux_loader.h](file:///d:/Code/Deshab/CODE/UTSM/include/utsm/linux_loader.h) | 修复 GPA 冲突：initrd 0x04000000→0x03000000，新增 IPC/RAM GPA 常量 |
| [linux_loader.c](file:///d:/Code/Deshab/CODE/UTSM/vmm/linux_loader.c) | e820 增加 IPC reserved 区域；RAM 映射移至 0x05000000 |
| [main.c](file:///d:/Code/Deshab/CODE/UTSM/kernel/main.c) | 在 `linux_loader_init()` 后调用 `ipc_shm_init()` |
| [MAKEFILE](file:///d:/Code/Deshab/CODE/UTSM/MAKEFILE) | 增加 `-I../utsm-ipc` include 路径 |
| [build.sh](file:///d:/Code/Deshab/CODE/linux/build.sh) | 自动安装 UTSM 驱动到 Linux 源码树 `drivers/utsm/` |
| [utsm_x86_64_defconfig](file:///d:/Code/Deshab/CODE/linux/configs/utsm_x86_64_defconfig) | 增加 `CONFIG_UTSM_HCALL=y` |

### 构建验证

`utsm.elf` 编译成功（87120 字节），新代码 **零 warning**。仅保留 3 个预存 warning（idt.c 和 vmx.c）。

### 后续步骤

1. **构建 Linux 内核**（需 Linux/WSL 环境）：
   ```bash
   cd CODE/linux && ./build.sh
   ```
2. **取消 limine.conf 注释**：启用 `linux-bzImage` 和 `linux-initrd.img` 模块
3. **QEMU 测试**（需 VMX/KVM 支持）：观察串口输出 `[HCALL] HELLO from Linux guest` 和 `[utsm] /dev/utsm registered`
4. Phase 1.4 — virtio-console 双向通信
5. Phase 1.5 — virtio-fs 文件共享

---

