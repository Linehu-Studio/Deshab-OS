#!/usr/bin/env bash
set -Eeuo pipefail
IMG="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
ROOT="${2:-/mnt/d/Code/DEAICUP/Deshab}"
DSK="${ROOT}/SYSTEM/system/deshab64/deshab.elf"
FUCK="${ROOT}/SYSTEM/system/deshab64/FUCK"
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount "${LOOP}p1" "$M"
cp -f "$DSK" "$M/system/deshab64/deshab.elf"
if [[ -f "$FUCK" ]]; then
    cp -f "$FUCK" "$M/system/deshab64/FUCK"
    echo "[patch] updated FUCK"
fi
sync
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo "[patch] updated $IMG /system/deshab64/deshab.elf"
