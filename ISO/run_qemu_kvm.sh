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
IMG="${IMG_OVERRIDE:-$SCRIPT_DIR/deshab-dev.img}"
SATA_IMG="${SATA_IMG_OVERRIDE:-$SCRIPT_DIR/../.build_tmp/sata_fat32_dsk.img}"
NVME_IMG="${NVME_IMG_OVERRIDE:-$SCRIPT_DIR/../.build_tmp/nvme_test.img}"
USB_IMG="${USB_IMG_OVERRIDE:-$SCRIPT_DIR/../.build_tmp/usb_test.img}"

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
    /usr/share/OVMF/OVMF_CODE_4M.fd \
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

# 4M OVMF requires a writable NVRAM vars file; derive it from the CODE image
# name and copy to a scratch location so the pristine firmware image is untouched.
OVMF_VARS_SRC="$(dirname "$OVMF")/$(basename "$OVMF" | sed 's/CODE/VARS/')"
OVMF_VARS=""
if [[ -f "$OVMF_VARS_SRC" ]]; then
    OVMF_VARS="${SCRIPT_DIR}/../.build_tmp/ovmf_vars.fd"
    cp -f "$OVMF_VARS_SRC" "$OVMF_VARS"
fi

# Accelerator selection: KVM preferred for nested VMX (Linux guest).
# Override with QEMU_ACCEL=tcg to fall back to TCG (no nested VMX, Linux guest
# will skip VMX init gracefully).
ACCEL="${QEMU_ACCEL:-kvm}"
# Hide CET from L1 CPUID. Hardware CET can still leak into L2 when nested
# KVM skips CET VMCS; qemu_disable_cet.c then tries KVM_SET_MSRS U_CET=0.
CPU="${QEMU_CPU:-host}"
if [[ "$ACCEL" == "kvm" ]]; then
    if [[ ! -e /dev/kvm ]]; then
        echo "[qemu] WARNING: /dev/kvm not found, KVM acceleration unavailable."
        echo "[qemu]   - On WSL2: ensure Windows 11 Pro/Enterprise (nested VMX needs it),"
        echo "[qemu]     and run 'wsl --update' + enable 'nestedVirtualization' in .wslconfig."
        echo "[qemu]   - On Linux: ensure kvm module is loaded and /dev/kvm permissions OK."
        echo "[qemu] Falling back to TCG (Linux guest will NOT run without nested VMX)."
        ACCEL="tcg"
    else
        # Prefer hiding IBT/SHSTK from L1 CPUID. Nested KVM still may leak
        # L0 U_CET into L2; qemu_disable_cet.so tries KVM_SET_MSRS next.
        if [[ "$CPU" == "host" ]]; then
            if "$QEMU" -cpu help 2>/dev/null | grep -qE '(^|[[:space:]])ibt([[:space:]]|$)' && \
               "$QEMU" -cpu help 2>/dev/null | grep -qE '(^|[[:space:]])shstk([[:space:]]|$)'; then
                CPU="host,-ibt,-shstk"
            fi
        fi
    fi
fi
if [[ "$ACCEL" == "tcg" ]]; then
    CPU="qemu64,-vmx"   # disable VMX CPUID under TCG (no nested virt support)
fi

# Nested KVM leaks this process's U_CET into L2. Disable SHSTK/IBT for
# QEMU only (arch_prctl), never a host-wide wrmsr of IA32_U_CET.
CET_SRC="${SCRIPT_DIR}/../CODE/linux/qemu_disable_cet.c"
CET_SO="${SCRIPT_DIR}/../CODE/linux/.cache/disable_cet.so"
if [[ -f "$CET_SRC" ]]; then
    mkdir -p "$(dirname "$CET_SO")"
    if [[ ! -f "$CET_SO" || "$CET_SRC" -nt "$CET_SO" ]]; then
        gcc -shared -fPIC -O2 -o "$CET_SO" "$CET_SRC" -ldl 2>/dev/null || true
    fi
fi
if [[ -f "$CET_SO" ]]; then
    export LD_PRELOAD="$CET_SO${LD_PRELOAD:+:$LD_PRELOAD}"
    export GLIBC_TUNABLES="glibc.cpu.hwcaps=-IBT,-SHSTK${GLIBC_TUNABLES:+:}${GLIBC_TUNABLES:-}"
    echo "[qemu] QEMU-process CET disabled (LD_PRELOAD + glibc tunables)"
fi

# Memory: Limine loads SYSTEM/boot modules into RAM. linux-rootfs (~768MB)
# plus linux-extra-rootfs (<4GiB) need a larger default than the old 4G.
# Cap to host RAM so WSL/8G boxes do not fail with "Cannot allocate memory".
if [[ -z "${QEMU_MEM:-}" ]]; then
    host_mib="$(awk '/MemAvailable:/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 0)"
    if [[ "$host_mib" -gt 14000 ]]; then
        MEM="12G"
    elif [[ "$host_mib" -gt 9000 ]]; then
        MEM="8G"
    elif [[ "$host_mib" -gt 6500 ]]; then
        MEM="6G"
    else
        MEM="4G"
        echo "[qemu] WARNING: only ${host_mib} MiB available; 6G+ preferred for extra-rootfs"
    fi
else
    MEM="$QEMU_MEM"
fi
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

# Optional NVMe test disk (disabled when the file is missing).
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

# Optional SATA FAT32 sidecar (AHCI test). Never attach it on the same
# ide.0 port as the boot disk. The boot image itself is AHCI (see below)
# so UTSM/DSK can parse the GPT ESP; the old sata_fat32_dsk.img sideca
# hid ESP DSK when it was the first FAT32 block device.
if [[ -f "$SATA_IMG" && "$(readlink -f "$SATA_IMG" 2>/dev/null || echo "$SATA_IMG")" != "$(readlink -f "$IMG" 2>/dev/null || echo "$IMG")" ]]; then
    echo "[qemu] SATA sidecar: $SATA_IMG (ide.1)"
    SATA_OPTS="-drive id=sata0,format=raw,file=$SATA_IMG,if=none -device ide-hd,drive=sata0,bus=ide.1"
else
    echo "[qemu] no extra SATA sidecar (boot disk is AHCI)"
    SATA_OPTS=""
fi

# Serial: default stdio; QEMU_SERIAL_LOG=path redirects to a file
# (required for Phase 0 Linux guest log capture in headless runs).
# QEMU_SERIAL_TCP=127.0.0.1:45455 makes COM1 a socket so the host can
# send '1'/'2' for the DSK session chooser while still logging output.
SERIAL="stdio"
SERIAL_ARGS=()
if [[ -n "${QEMU_SERIAL_TCP:-}" ]]; then
    tcp="${QEMU_SERIAL_TCP}"
    host="${tcp%%:*}"
    port="${tcp##*:}"
    logopt=""
    if [[ -n "${QEMU_SERIAL_LOG:-}" ]]; then
        logopt=",logfile=${QEMU_SERIAL_LOG},logappend=on"
    fi
    SERIAL_ARGS=(-chardev "socket,id=com1,host=${host},port=${port},server=on,wait=off${logopt}" -serial chardev:com1)
    echo "[qemu] Serial: tcp:${tcp} log=${QEMU_SERIAL_LOG:-none}"
elif [[ -n "${QEMU_SERIAL_LOG:-}" ]]; then
    SERIAL="file:$QEMU_SERIAL_LOG"
    SERIAL_ARGS=(-serial "$SERIAL")
    echo "[qemu] Serial: $SERIAL"
else
    SERIAL_ARGS=(-serial "$SERIAL")
    echo "[qemu] Serial: $SERIAL"
fi

DISPLAY_OPTS=()
if [[ -n "${QEMU_DISPLAY:-}" ]]; then
    DISPLAY_OPTS=(-display "$QEMU_DISPLAY")
    echo "[qemu] Display: $QEMU_DISPLAY"
fi

MONITOR_OPTS=()
if [[ -n "${QEMU_MONITOR:-}" ]]; then
    MONITOR_OPTS=(-monitor "$QEMU_MONITOR")
    echo "[qemu] Monitor: $QEMU_MONITOR"
fi

PFLASH_VARS=()
if [[ -n "$OVMF_VARS" ]]; then
    PFLASH_VARS=(-drive if=pflash,format=raw,file="$OVMF_VARS")
fi

exec "$QEMU" \
    -accel "$ACCEL" \
    -machine q35 \
    -m "$MEM" \
    -smp "$SMP" \
    -cpu "$CPU" \
    "${DISPLAY_OPTS[@]}" \
    "${MONITOR_OPTS[@]}" \
    "${SERIAL_ARGS[@]}" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    "${PFLASH_VARS[@]}" \
    -drive if=none,id=osdisk,format=raw,file="$IMG" \
    -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 \
    $SATA_OPTS \
    $NVME_OPTS \
    $USB_OPTS \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -boot menu=on
