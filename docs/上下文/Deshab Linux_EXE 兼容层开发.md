# Deshab Linux/EXE 兼容层开发

## 需求

- Linux 程序兼容层（依赖双内核架构，WSL 互操作模式）
- EXE 兼容层直接写在 UTSM 里（PE32+ 64位原生 Ring0 执行 + PE32 32位 x86 解释器）
- cmd.exe 编译到 `SYSTEM/system/deshab64`
- 桌面命令行1: shell.elf（原生 + `linux` 命令运行 Linux 程序）
- 桌面命令行2: cmd.elf（Windows 风格 + PE/EXE 兼容）

---

## PE/EXE 兼容层（UTSM 内置）

### 核心文件

| 文件 | 职责 |
|------|------|
| `CODE/UTSM/pe/pe_loader.c` / `.h` | PE 解析、映射、重定位(R_X86_64_64/32/32S/PC32)、import 解析 |
| `CODE/UTSM/pe/pe_shim.c` / `.h` | Windows API shim（`__attribute__((ms_abi))`，`__builtin_setjmp`/`__builtin_longjmp`）|
| `CODE/UTSM/pe/x86emu32.c` / `.h` | x86-32 解释器：支持 push/call/add esp/ret/mov/xor/ModRM 解码/IAT 拦截 |
| `CODE/UTSM/pe/pe_service.c` | 统一服务：`pe_service_load`/`pe_service_run`/`pe_service_unload` |

### PE 加载流程

- **PE32+**: 原生 Ring0 执行，MS x64 ABI shim
- **PE32**: x86emu32 解释器执行，`preferred_base` 加载（0x00400000），专用大缓冲区含栈和堆
- `pe_image_info` 结构体返回加载信息（entry_point, image_base, image_size, is_pe32_plus）
- 通过 `dsk_boot_context.reserved[4]` 暴露给 cmd.elf

### 关键结构

```c
// pe.h - PE 服务接口
struct pe_service {
    int (*load)(const char *name, u8 **out_data, u64 *out_size, pe_image_info *info);
    int (*run)(u8 *data, u64 size, pe_image_info *info, int argc, char **argv);
    void (*unload)(u8 *data, u64 size, pe_image_info *info);
};

// pe_loader.h - PE 结构体（DOS/NT/File/Optional32/64/Section/Import Descriptor）
#define PE_DOS_MAGIC    0x5A4D
#define PE_NT_MAGIC     0x00004550
#define PE32_MAGIC      0x010B
#define PE32_PLUS_MAGIC 0x020B
```

---

## Linux 兼容层（双内核 park-and-resume）

### 架构

```
shell 输入 `linux ls` → g_lxc_svc->exec("/bin/ls") → 写 IPC 请求
→ linux_resume() vmresume 唤醒 Linux guest
→ daemon fork+exec ls → pipe 捕获 stdout
→ 发 EXEC_STDOUT/EXIT → HLT park → UTSM 读取响应返回 shell
```

### 核心文件

| 组件 | 文件 | 说明 |
|------|------|------|
| park-and-resume | `vmm/linux_resume.c`, `vmm/linux_boot.c`, `vmm/vmexit.c` | Linux guest HLT 时 park（推进 RIP），UTSM 通过 vmresume 唤醒 |
| IPC exec 协议 | `CODE/utsm-ipc/ipc_proto.h` | EXEC_REQUEST/STDOUT/STDERR/EXIT/READY 消息类型 + 载荷 |
| Linux compat 服务 | `vmm/linux_compat.c`, `include/utsm/linux_compat.h` | exec(): 写 IPC 请求 → linux_resume → 读取响应 |
| Boot context | `kernel/dsk_loader.c` | reserved[4]=pe_service, reserved[5]=linux_compat_service |
| shell `linux` 命令 | `CODE/shell/main.c` | `linux ls -la` → /bin/ls，支持绝对路径 |
| Linux 驱动 ioctl | `CODE/linux/patches/utsm_hcall.c` | RECV_MSG/SEND_MSG/PARK/GET_READY ioctl |
| Linux exec daemon | `CODE/linux/initramfs/bin/utsm_exec_daemon.c` | fork+exec+pipe 捕获输出，通过 IPC 回传 |
| init 脚本 | `CODE/linux/initramfs/init` | 启动 daemon，daemon 发 EXEC_READY 后 HLT park |

### 关键全局变量

```c
// vmm.c
volatile int g_guest_parked;       // Linux guest 是否已 park
volatile int g_guest_terminated;   // self-test guest 是否已终止

// linux_boot.c
volatile u64 g_saved_host_rsp;     // linux_launch/linux_resume 保存的 host RSP
volatile u64 g_saved_return_rip;   // linux_launch/linux_resume 保存的返回 RIP
```

### IPC 消息类型（ipc_proto.h 扩展）

```c
#define UTSMB_MSG_EXEC_REQUEST  0x10  // {cmd_path[256], argc, argv[8][128]}
#define UTSMB_MSG_EXEC_STDOUT   0x11  // {data[N], len}
#define UTSMB_MSG_EXEC_STDERR   0x12  // {data[N], len}
#define UTSMB_MSG_EXEC_EXIT     0x13  // {exit_code}
#define UTSMB_MSG_EXEC_READY    0x14  // daemon 就绪
```

---

## 第一阶段：Linux guest 底层基础设施

### EPT 动态页映射

- `vmexit.c` handle_ept_violation 区分 RAM（按需分配 HPA）和 MMIO（路由到设备模拟）
- 需从 guest 指令解码 MMIO 访问大小和数据

### virtio-mmio 框架

| 文件 | 说明 |
|------|------|
| `vmm/virtio_mmio.c` / `.h` | 寄存器分发 + virtqueue 状态机 + 后端路由 |
| `vmm/virtio_blk.c` | virtio-blk 后端，桥接 guest 块请求到 UTSM block API |
| `vmm/virtio_net.c` | virtio-net 后端占位 |

**GPA 布局**：
- `0x0A000000-0x0A0001FF`: virtio-mmio #0 (blk, IRQ5)
- `0x0A000200-0x0A0003FF`: virtio-mmio #1 (net, IRQ6)

**virtqueue 关键 API**：
```c
void virtio_queue_get_ptrs(virtio_dev_state *dev, int qidx,
    u16 *avail_idx, u16 *used_idx, u64 *desc_addr, u64 *avail_addr, u64 *used_addr);
```

### vmexit dispatch 接线

| Exit Reason | Handler | 说明 |
|-------------|---------|------|
| EXIT_CPUID | handle_cpuid | host passthrough + 屏蔽 VMX/hypervisor 位 |
| EXIT_IO_INSTRUCTION | handle_io | COM1/PIC/PIT 模拟 |
| EXIT_INTERRUPT_WINDOW | 新增 | guest 开中断时注入 pending IRQ |
| EXIT_VMX_PREEMPTION_TIMER | 新增 | 1ms 周期 exit |
| resume 路径 | vmexit_before_resume() | arm timer + 串口 RX 轮询 + PIT tick + IRQ 注入 |

**virtio IRQ**：QUEUE_NOTIFY 后 `vmx_guest_queue_irq(0x30+irq)`，blk→0x35，net→0x36

### initramfs

- `build.sh`: busybox 1.35.0 静态下载 + gen_init_cpio 打包
- init: daemon 缺失时 `setsid cttyhack sh` 回退 ttyS0 交互 shell
- `/dev/console`, `/dev/ttyS0` 节点 + 12 个 applet symlink

### linux_boot 集成

- `linux_launch()` 调用 `virtio_mmio_init()` + 启用 PIN_VMX_PREEMPTION_TIMER + 初始 arm preemption timer

---

## cmd.elf

Windows 风格命令行，支持 PE/EXE 执行。

### 关键功能
- FAT32 根目录 + `/bin` 子目录 PE 查找（PATH 查找：先根目录，再 /bin）
- `pe` 命令：显式运行 PE 程序
- `peinfo` 命令：解析 PE 头显示 DOS/NT/File/Optional Header、节区表、导入表
- 内建命令：help, echo, cls, dir, ver, exit, type, copy, del, ren, cd, color
- 通过 `reserved[4]` 获取 pe_service，`reserved[5]` 获取 linux_compat_service

### PE 样本部署

| 文件 | 格式 | 位置 |
|------|------|------|
| hello64.exe | PE32+ (1536B) | SYSTEM/bin/ |
| hello32.exe | PE32 (2048B) | SYSTEM/bin/ |
| cmd.exe | PE32+ (1536B) | SYSTEM/bin/ |

---

## 构建验证

全量构建成功，新代码零警告：
- utsm.elf, deshab.elf, shell.elf (791KB), cmd.elf (490KB), desktop.elf (140KB)
- FAT32 镜像含 CMD ELF

## 待办（需 WSL）

Linux 内核需在 WSL 构建（`CODE/linux/build.sh`，约 30 分钟）。构建后取消注释 `SYSTEM/limine/limine.conf` 中 bzImage/initrd module 条目即可启用。Linux 未构建时 `linux` 命令显示"兼容层不可用"。

### WSL 构建命令

```bash
sudo apt update && sudo apt install -y build-essential bc flex bison libelf-dev libssl-dev cpio
cd ~
curl -L -o linux-6.6.tar.xz https://mirrors.tuna.tsinghua.edu.cn/kernel/v6.x/linux-6.6.tar.xz
tar xf linux-6.6.tar.xz && cd linux-6.6
# 安装 UTSM 驱动 + 配置 + 编译
cp /mnt/d/Code/Deshab/CODE/linux/configs/utsm_x86_64_defconfig .config
make olddefconfig
make -j$(nproc) KCFLAGS='-std=gnu17' bzImage
# 拷贝产物 + 构建 initramfs
mkdir -p /mnt/d/Code/Deshab/SYSTEM/boot
cp arch/x86/boot/bzImage /mnt/d/Code/Deshab/SYSTEM/boot/linux-bzImage
cd /mnt/d/Code/Deshab/CODE/linux && ./build.sh
```

**注意**：gcc 15 默认 C23，需 `KCFLAGS='-std=gnu17'` 修复与 Linux 6.6 的兼容性。
