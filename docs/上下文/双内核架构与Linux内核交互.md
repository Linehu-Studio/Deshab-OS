# 双内核架构与Linux内核交互

## 需求

UTSM + Linux 双内核架构，运行 Linux 内核虚拟机，修改 Linux 内核增加双核交互。参考设计: `D:\Code\ZT5`。

## 架构决策

- **虚拟化方案**: VMX/EPT 完整虚拟化
- **分工**: Linux 独占硬件，UTSM 作为监控核（hypervisor 模型）
- **Linux 版本**: 6.6 LTS
- **第一阶段范围**: virtio-console + 文件共享

---

## UTSM 目录结构

```
CODE/UTSM/
├── arch/x86_64/       # boot.S, idt.c, io.c, serial.c, limine.h
├── core/              # capability.c, dirty.c, dmp.c, process.c, segment.c, utsm_init.c
├── crypto/            # kdf.c, stream.c, tweak.c
├── dkm/               # boot_modules.c, console_early.c, dkm_core.c, kernel_api.c, manifest.c
├── include/utsm/      # 所有头文件（arena.h, dkm.h, vmm.h, ept.h, hypercall.h, ipc_shm.h 等）
├── kernel/            # block.c, dsk_loader.c, log.c, main.c, net.c, panic.c
├── mm/                # arena.c, dma.c
├── pe/                # pe_loader.c, pe_shim.c, x86emu32.c, pe_service.c
├── vmm/               # vmx.c, ept.c, vmexit.c, vmm.c, vmexit_asm.S, linux_boot.c,
│                      # linux_loader.c, linux_resume.c, ipc_shm.c, hypercall.c,
│                      # linux_compat.c, virtio_mmio.c, virtio_blk.c, virtio_net.c
├── MAKEFILE, linker.ld
```

---

## Phase 1.1: VMX 基础设施

### 核心文件

| 文件 | 职责 |
|------|------|
| `vmm/vmx.c` / `include/utsm/vmx.h` | VMXON/VMXOFF, VMCS 读写, EPT 页表管理 |
| `vmm/ept.c` / `include/utsm/ept.h` | EPT 4级页表创建/映射/遍历, `ept_gpa_to_hpa()` |
| `vmm/vmm.c` | VMM 初始化, VMCS 配置(Host/Guest/Controls), 自测程序 |
| `vmm/vmexit_asm.S` | VM-Exit 汇编入口: 保存/恢复全部 16 个 guest GPR 到 `g_guest_regs` |
| `vmm/vmexit.c` | VM-Exit 分发: HLT/EPTViolation/异常/CPUID/IO/MSR/VMCALL |

### 关键 VMCS 字段（vmx.h）

```c
// Host State
VMCS_HOST_CR3, VMCS_HOST_RIP, VMCS_HOST_RSP, VMCS_HOST_GDTR_BASE, VMCS_HOST_TR_SEL
// Guest State
VMCS_GUEST_CR0/CR3/CR4, VMCS_GUEST_RIP, VMCS_GUEST_RFLAGS
VMCS_GUEST_ES/CS/SS/DS/FS/GS/TR/LDTR_SEL
VMCS_GUEST_GDTR/LDTR_BASE/LIMIT
VMCS_GUEST_IA32_EFER, VMCS_HOST_IA32_EFER
// Controls
VMCS_PIN_CONTROLS, VMCS_CPU_CONTROLS, VMCS_CPU_SECONDARY
VMCS_ENTRY_CONTROLS, VMCS_EXIT_CONTROLS, VMCS_EPT_POINTER
```

### EPT 映射

```c
void ept_identity_map(u64 gpa, u64 permissions);  // 恒等映射 GPA→HPA
u64  ept_gpa_to_hpa(u64 gpa);                      // 4 级 EPT walk（支持 4KB/2MB/1GB 页）
```

### 自测流程

1. `vmm_init()` → VMXON + EPT 初始化
2. `vmm_self_test()` → 创建测试 guest VMCS, vmlaunch
3. Guest 执行 HLT → vmexit handler 设置 `g_guest_terminated=1`
4. 汇编检测终止标志 → 恢复 host 栈返回

**注意**: VMX 需要 VT-x 硬件支持，QEMU TCG 不支持 VMX。WHPX 在 OVMF 下崩溃。测试需真实硬件。

---

## Phase 1.2: Linux Loader

### 核心文件

| 文件 | 职责 |
|------|------|
| `vmm/linux_loader.c` / `include/utsm/linux_loader.h` | bzImage 解析, boot_params 设置, EPT 映射 |
| `vmm/linux_boot.c` | Linux guest VMCS 配置 + vmlaunch |

### GPA 内存布局

```
0x00000000-0x003FFFFF  低 RAM (boot_params, GDT 等)
0x00400000             boot_params GPA
0x00600000             Guest GDT
0x01000000             bzImage kernel
0x03000000             initrd (原 0x04000000, 修复冲突后移至 0x03000000)
0x04000000-0x04FFFFFF IPC 共享内存 (1MB, e820 reserved)
0x05000000-0x08FFFFFF Guest RAM (64MB)
0x0A000000-0x0A0001FF virtio-mmio #0 (blk)
0x0A000200-0x0A0003FF virtio-mmio #1 (net)
```

### Linux Boot Protocol

```c
struct setup_header {
    u8  setup_sects, root_flags, syssize[4];
    u16 ram_size, vid_mode, root_dev;
    u8  jump[2], header[4], version[2];
    u32 realmode_swtch, start_sys, kernel_version;
    u8  type_of_loader;
    u8  loadflags;       // LOADED_HIGH bit
    u16 initrd_addr_max;
    // ... ext_* fields
};

struct boot_params {
    struct screen_info screen_info;
    struct apm_bios_info apm_bios_info;
    u8  pad2[4];
    u64 tboot_addr;
    u8  pad3[24];
    struct e820_entry e820_map[E820_MAX_ENTRIES];
    // ... setup_header at end
};
```

### Linux Boot 流程

1. 从 Limine boot module 获取 bzImage/initrd
2. 解析 setup_header, 验证 magic `HdrS`
3. 设置 boot_params: e820 内存图 + cmdline + initrd 地址/大小
4. EPT 映射所有 GPA 区域
5. VMCS 配置 guest 状态 (CR0/3/4, segments, EFER.LME+LMA)
6. vmlaunch → guest 进入 64 位模式

---

## Phase 1.3: IPC 骨架

### IPC 共享内存（1MB @ GPA 0x04000000）

```c
struct utsm_ipc_shm {
    u32 magic;           // UTSM_IPC_MAGIC
    u32 version;
    u32 host_to_guest_head, host_to_guest_tail;
    u32 guest_to_host_head, guest_to_host_tail;
    u8  ring_buf[UTSM_IPC_RING_SIZE];
    u8  payload_pool[UTSM_IPC_PAYLOAD_SIZE];  // 1MB, 未用
};
```

### Hypercall ABI（VMCALL）

Guest 通过 `vmcall(rax=UTSM_HCALL_MAGIC, rdi=op, rsi=arg1, rdx=arg2)` 调用。

```c
#define UTSMB_HCALL_PING          0x01
#define UTSMB_HCALL_HELLO         0x02
#define UTSMB_HCALL_SHM_INFO      0x03
#define UTSMB_HCALL_CAP_VALIDATE  0x04
#define UTSMB_HCALL_UTRW_READ     0x05
#define UTSMB_HCALL_UTRW_WRITE    0x06
#define UTSMB_HCALL_DRR_CHECKPOINT 0x07
#define UTSMB_HCALL_CONSOLE_WRITE 0x08
#define UTSMB_HCALL_CONSOLE_READ  0x09
```

### Linux 侧驱动（utsm_hcall.c）

- `/dev/utsm` 字符设备
- ioctl: RECV_MSG / SEND_MSG / SHM_MMAP
- VMCALL 汇编封装

### UTSM 启动顺序（kernel/main.c）

```c
vmm_init() → vmm_self_test() → ipc_shm_init() → linux_loader_init()
→ linux_launch() → linux_compat_init() → dsk_load_and_jump()
```

---

## Limine 配置

`SYSTEM/limine/limine.conf` 中 Linux 模块默认注释（文件不存在时 Limine 启动失败）：

```
# /boot/linux-bzImage: cmdline="fuck:config console=ttyS0 nohlt idle=poll"
# /boot/linux-initrd.img:
```

build.sh 成功后自动取消注释。

---

## Linux 构建

`CODE/linux/build.sh` 脚本: 克隆 Linux 6.6 → 安装 UTSM 驱动 → defconfig → bzImage → initramfs → 拷贝产物。

需 WSL 环境，`KCFLAGS='-std=gnu17'` 修复 gcc 15 C23 兼容性。

---

## 构建验证

- utsm.elf 编译成功（87120 字节），新代码零 warning
- VMX 自测在 TCG 下优雅降级（打印 "VMX not available"）
- 真实 VT-x 硬件测试待执行
