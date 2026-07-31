# Linux 6.6 LTS for UTSM Dual-Kernel Architecture

## Overview

This directory contains the Linux 6.6 LTS configuration and build scripts for the
UTSM + Linux dual-kernel architecture. Linux runs as a VMX non-root guest under
the UTSM hypervisor (VMX root).

## Prerequisites

- **WSL** (Windows Subsystem for Linux) with Ubuntu 22.04+ installed
  ```powersShell
  wsl --install -d Ubuntu-22.04
  ```
- Build dependencies (inside WSL):
  ```bash
  sudo apt update
  sudo apt install -y build-essential bc flex bison libelf-dev libssl-dev cpio gzip git
  ```
- ~3GB free disk space

## Building Linux

### 1. From WSL

```bash
# Navigate to the project (adjust path for your WSL mount)
cd /mnt/d/Code/Deshab/CODE/linux

# Make the build script executable
chmod +x build.sh

# Run the build (clones Linux 6.6 LTS, configures, builds)
./build.sh
```

The build will:
1. Clone Linux 6.6 LTS source to `CODE/linux/src/`
2. Apply UTSM patches from `CODE/linux/patches/` (if any)
3. Configure with `configs/utsm_x86_64_defconfig`
4. Build `bzImage`
5. Copy to `SYSTEM/boot/linux-bzImage`
6. Build initramfs if `CODE/linux/initramfs/` exists

### 2. Enable in Limine

After building, uncomment the Linux module entries in `SYSTEM/limine/limine.conf`:

```
    module_path: boot():/boot/linux-bzImage
    module_cmdline: linux:bzImage
    module_path: boot():/boot/linux-initrd.img
    module_cmdline: linux:initrd
```

### 3. Rebuild the disk image

```PowerShell
cd d:\Code\Deshab
.\build.bat
```

### 4. Test with QEMU

**Note:** VMX requires hardware virtualization support. QEMU's TCG (software
emulation) does NOT support VMX instructions. To test the VMX path:

- **Real hardware**: Boot on an Intel machine with VT-x enabled in BIOS
- **QEMU with WHPX**: `qemu-system-x86_64 -accel whpx -cpu max` (may not expose
  nested VMX to the guest)
- **QEMU with KVM** (Linux host only): `qemu-system-x86_64 -enable-kvm -cpu host`

Without VMX, UTSM gracefully falls back to the normal DSK boot flow.

## Configuration Details

The `utsm_x86_64_defconfig` enables:
- 64-bit kernel with SMP support
- Serial console (8250/16550 at 0x3F8, 115200 baud)
- virtio: console, block, net, fs (for UTSM host backends)
- ext4, FAT, tmpfs, proc, sysfs filesystems
- No module loading (all drivers built-in for Phase 1)
- No EFI stub (UTSM uses custom VMX boot protocol)
- Debug info and printk timestamps

## File Structure

```
CODE/linux/
├── configs/
│   └── utsm_x86_64_defconfig   # Kernel configuration
├── patches/                     # UTSM-specific kernel patches
│   ├── utsm-hcall.patch         # /dev/utsm hypercall driver (Phase 1.3)
│   ├── utsm-ipc.patch           # Shared memory IPC driver (Phase 1.3)
│   └── utsm-virtio.patch        # virtio enhancements (Phase 1.4+)
├── initramfs/                   # Minimal initramfs contents (optional)
├── build.sh                     # Build script (run in WSL/Linux)
└── README.md                    # This file
```

## Boot Flow

```
Limine (UEFI)
  → utsm.elf (VMX root)
    → vmm_init() + vmm_self_test()
    → linux_loader_init() (parse bzImage, set up boot_params, EPT)
    → linux_launch() (VMCS config + vmlaunch)
      → Linux 6.6 startup_64 (VMX non-root)
        → Serial console output
        → virtio-console ↔ UTSM
        → virtio-fs mount /utsm
        → User space (init/shell)
```

## Guest Networking (virtio-net)

UTSM vmm 侧模拟一块 virtio-net 网卡（virtio-mmio 模型，GPA `0xF4001000`，
guest ISA IRQ6），数据路径桥接到 host 真实网卡（e1000/virtio-net DKM 驱动，
经 `kernel_api.net` 的 `tx`/`rx_poll` 收发帧）。guest 内核 cmdline 已自动注入：

```
virtio_mmio.device=4K@0xF4001000:6
```

`utsm_x86_64_defconfig` 已启用 `CONFIG_VIRTIO_NET=y`（内置，非模块），
内核启动后应出现 `eth0`（virtio-mmio 无 PCI 拓扑，网卡名固定为 eth0）：

```bash
# 确认网卡已识别（dmesg 应能看到 virtio_net  probe 日志）
ip link show

# 拉起接口
ip link set eth0 up

# 方式一：DHCP（QEMU user-net/slirp 环境，网关 10.0.2.2，DNS 10.0.2.3）
dhcpcd eth0
# 或：dhclient eth0

# 方式二：静态地址（无 DHCP 服务器时）
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2
echo "nameserver 10.0.2.3" > /etc/resolv.conf

# 验证连通性
ping -c3 10.0.2.2        # QEMU slirp 网关（永远在线，适合冒烟测试）
ping -c3 10.0.2.3        # slirp 内建 DNS
pacman -Sy               # 联网同步包数据库
```

注意事项：

- **MAC 共用**：guest virtio-net 的 MAC 与 host e1000 相同（UTSM 桥接模型，
  config 空间直接回填 host 网卡 MAC）。QEMU user-net 下 slirp 按 MAC 分配
  `10.0.2.15`；若 host Deshab 原生网络栈同时用该网卡联网，两者会抢同一
  DHCP 租约，实测时请只让一侧做 DHCP（或 host 完全不用网络）。
- **MTU**：后端按标准 1500 MTU 转发，不支持 GSO/TSO（feature 未协商，
  guest 自动走软件分片/checksum 路径）。
- **无中断收包**：RX 走 UTSM 周期轮询（VMX preemption timer，~1kHz）
  填充 guest RX ring 并注入 IRQ6；大流量下串口统计日志每 512 帧打印一行
  `[VNET] stat ...`（rx_filled / rx_drop_nobuf / tx_sent / tx_drop），
  可用于判断包在哪一段丢失。
- 若 `ip link` 看不到 eth0：检查 dmesg 中 `virtio_mmio` probe 是否成功，
  以及 host 串口日志是否有 `[VNET] bound to UTSM net device 0`
  （host 侧 e1000/virtio-net DKM 驱动需先注册 netdev）。
