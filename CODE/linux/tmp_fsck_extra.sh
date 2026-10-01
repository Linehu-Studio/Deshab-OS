#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
IMG="$ROOT/ISO/deshab-dev.img"
killall qemu-system-x86_64 2>/dev/null || true
sleep 1
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount "${LOOP}p1" "$M"
EXTRA="$M/boot/linux-extra-rootfs.img"
echo "[fsck] $EXTRA"
ls -l "$EXTRA"
e2fsck -fy "$EXTRA" || true
sync
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo FSCK_DONE
