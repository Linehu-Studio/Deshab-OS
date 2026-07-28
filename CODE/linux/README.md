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
