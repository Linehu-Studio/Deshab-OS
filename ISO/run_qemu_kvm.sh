#!/usr/bin/env bash
# Deshab QEMU launcher for WSL2/Linux hosts with KVM acceleration.
#
# VSCode integration requires nested VMX (Linux 6.6 guest runs inside UTSM's
# VMX root). WHPX (Windows) and TCG both lack nested VMX, so the Linux guest
# cannot start under them. Use this script from a WSL2 shell or a native Linux
# host that exposes /dev/kvm with VMX-enabled nested virtualization.
#
# Prereqs:
#   - WSL2 (Windows 11 Pro/Enterprise for nested VMX; Home edition lacks it)
#     + `wsl --install` + ensure kernel supports nested virt
#     + /dev/kvm exists and is readable/writable
#   - Or native Linux host with KVM
#   - qemu-system-x86_64 in PATH
#   - OVMF firmware at one of the paths probed below
#
# Usage:
#   ./run_qemu_kvm.sh              # default: -m 4G -smp 2, accel kvm
#   QEMU_MEM=6G ./run_qemu_kvm.sh  # override memory
#   QEMU_SMP=4 ./run_qemu_kvm.sh   # override vcpu count
#   QEMU_ACCEL=tcg ./run_qemu_kvm.sh # force TCG fallback (no nested VMX)
#
# See ISO/run_qemu.bat for Windows host fallback (WHPX/TCG, no Linux guest).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMG="$SCRIPT_DIR/deshab-dev.img"
SATA_IMG="$SCRIPT_DIR/../.build_tmp/sata_fat32_dsk.img"
NVME_IMG="$SCRIPT_DIR/../.build_tmp/nvme_test.img"
USB_IMG="$SCRIPT_DIR/../.build_tmp/usb_test.img"

if [[ ! -f "$IMG" ]]; then
    echo "[qemu] Image not found: $IMG"
    echo "[qemu] Run build.bat first (default builds both dev and release),"
    echo "[qemu] or build dev only: build.bat -Variant dev"
    exit 1
fi

# Locate qemu-system-x86_64
QEMU="${QEMU_BIN:-}"
if [[ -z "$QEMU" ]]; then
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        QEMU="$(command -v qemu-system-x86_64)"
    fi
fi
if [[ -z "$QEMU" ]]; then
    echo "[qemu] qemu-system-x86_64 was not found."
    echo "[qemu] Install QEMU: apt install qemu-system-x86 (Debian/Ubuntu)"
    echo "[qemu]               or pacman -S qemu-system-x86 (Arch/WSL2)"
    exit 1
fi

# Locate OVMF firmware
OVMF=""
for p in \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/qemu/OVMF.fd \
    /usr/share/qemu-efi/QEMU_UEFI_CODE.fd ; do
    if [[ -f "$p" ]]; then OVMF="$p"; break; fi
done
if [[ -z "$OVMF" ]]; then
    echo "[qemu] OVMF firmware was not found."
    echo "[qemu] Install ovmf: apt install ovmf (Debian/Ubuntu) or pacman -S edk2-ovmf (Arch)."
    exit 1
fi

# Accelerator selection: KVM preferred for nested VMX (Linux guest).
# Override with QEMU_ACCEL=tcg to fall back to TCG (no nested VMX, Linux guest
# will skip VMX init gracefully).
ACCEL="${QEMU_ACCEL:-kvm}"
CPU="host"
if [[ "$ACCEL" == "kvm" ]]; then
    if [[ ! -e /dev/kvm ]]; then
        echo "[qemu] WARNING: /dev/kvm not found, KVM acceleration unavailable."
        echo "[qemu]   - On WSL2: ensure Windows 11 Pro/Enterprise (nested VMX needs it),"
        echo "[qemu]     and run 'wsl --update' + enable 'nestedVirtualization' in .wslconfig."
        echo "[qemu]   - On Linux: ensure kvm module is loaded and /dev/kvm permissions OK."
        echo "[qemu] Falling back to TCG (Linux guest will NOT run without nested VMX)."
        ACCEL="tcg"
    fi
fi
if [[ "$ACCEL" == "tcg" ]]; then
    CPU="qemu64,-vmx"   # disable VMX CPUID under TCG (no nested virt support)
fi

# Memory and CPU topology: VSCode + Electron + GUI stack needs >= 4GB.
MEM="${QEMU_MEM:-4G}"
SMP="${QEMU_SMP:-2}"

# Detect whether Linux compat layer is enabled (bzImage module present in IMG).
LINUX_ENABLED=0
if grep -q 'module_path: boot():/boot/linux-bzImage' \
        "$SCRIPT_DIR/../SYSTEM/limine/limine.conf" 2>/dev/null ; then
    # Check the line is uncommented (not prefixed with '#')
    if grep -E '^\s*module_path: boot\(\):/boot/linux-bzImage' \
            "$SCRIPT_DIR/../SYSTEM/limine/limine.conf" >/dev/null 2>&1 ; then
        LINUX_ENABLED=1
    fi
fi

echo "[qemu] Using: $QEMU"
echo "[qemu] UEFI:  $OVMF"
echo "[qemu] Image: $IMG"
echo "[qemu] Accel: $ACCEL"
echo "[qemu] CPU:   $CPU"
echo "[qemu] Mem:   $MEM"
echo "[qemu] SMP:   $SMP"
if [[ "$LINUX_ENABLED" == "1" ]]; then
    echo "[qemu] Linux compat: ENABLED (modules present)"
    if [[ "$ACCEL" == "kvm" ]]; then
        echo "[qemu]   KVM nested VMX: ready for Linux guest (VSCode integration path)"
    else
        echo "[qemu]   WARNING: $ACCEL does not support nested VMX."
        echo "[qemu]   Linux guest will NOT run; UTSM will skip VMX init."
    fi
else
    echo "[qemu] Linux compat: disabled (build via CODE/linux/build.sh to enable)"
fi

# Optional NVMe test disk
NVME_OPTS=""
if [[ -f "$NVME_IMG" ]]; then
    echo "[qemu] NVMe:  $NVME_IMG"
    NVME_OPTS="-drive if=none,id=nvme0,file=$NVME_IMG,format=raw -device nvme,drive=nvme0,serial=deadbeef"
fi

# Optional USB xHCI test disk
USB_OPTS=""
if [[ -f "$USB_IMG" ]]; then
    echo "[qemu] USB:   $USB_IMG"
    USB_OPTS="-device qemu-xhci,id=xhci0 -drive id=usb0,if=none,file=$USB_IMG,format=raw -device usb-storage,bus=xhci0.0,drive=usb0"
fi

# Optional SATA FAT32 disk (AHCI test)
if [[ -f "$SATA_IMG" ]]; then
    echo "[qemu] SATA:  $SATA_IMG"
    SATA_OPTS="-drive id=sata0,format=raw,file=$SATA_IMG,if=none -device ide-hd,drive=sata0,bus=ide.0"
else
    echo "[qemu] SATA image not found, booting without block device"
    SATA_OPTS=""
fi

exec "$QEMU" \
    -accel "$ACCEL" \
    -machine q35 \
    -m "$MEM" \
    -smp "$SMP" \
    -cpu "$CPU" \
    -serial stdio \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive format=raw,file="$IMG",if=virtio \
    $SATA_OPTS \
    $NVME_OPTS \
    $USB_OPTS \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -boot menu=on
