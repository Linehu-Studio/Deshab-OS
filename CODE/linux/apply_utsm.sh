#!/usr/bin/env bash
# Apply the current SYSTEM/boot/utsm.elf to the ESP of one or more GPT images,
# then prove the in-image copy matches (md5 + FLUSH_N marker). Idempotent.
# ESP-only: Limine loads the kernel via boot(): from the FAT32 ESP.
# Usage: apply_utsm.sh IMG [IMG...]
set -Eeuo pipefail

ROOT=/mnt/d/Code/DEAICUP/Deshab
SRC="$ROOT/SYSTEM/boot/utsm.elf"
[[ -f "$SRC" ]] || { echo "missing $SRC"; exit 2; }

EXP=$(md5sum "$SRC" | cut -d' ' -f1)
FLUSH=$(strings -a "$SRC" | grep -c 'VGPU\] FLUSH_N' || true)
echo "source: md5=$EXP size=$(stat -c %s "$SRC") flush_marker=$FLUSH"
if [[ "$FLUSH" -lt 1 ]]; then
    echo "FATAL: source utsm.elf has no FLUSH_N marker; verify cannot judge Plasma scanout"
    exit 2
fi

rc=0
for img in "$@"; do
    name=$(basename "$img")
    if [[ ! -f "$img" ]]; then
        echo "$name: MISSING"
        rc=1
        continue
    fi
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    if ! mount "${L}p1" "$M" 2>/dev/null; then
        echo "$name: mount p1 FAILED"
        rmdir "$M"
        losetup -d "$L"
        rc=1
        continue
    fi
    cp -f "$SRC" "$M/boot/utsm.elf"
    sync
    got=$(md5sum "$M/boot/utsm.elf" | cut -d' ' -f1)
    n=$(strings -a "$M/boot/utsm.elf" | grep -c 'VGPU\] FLUSH_N' || true)
    if [[ "$got" == "$EXP" && "$n" -ge 1 ]]; then
        echo "$name: APPLIED md5=$got flush_marker=$n"
    else
        echo "$name: VERIFY-FAILED md5=$got flush_marker=$n"
        rc=1
    fi
    umount "$M"
    rmdir "$M"
    losetup -d "$L"
done
exit $rc