#!/usr/bin/env bash
# Dump the first 64 bytes of one lib inside extra-rootfs (native img).
set -uo pipefail
IMG="${1:?img}"
NAME="${2:?libname}"
L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M" || { echo ESP_FAIL; exit 1; }
R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-extra-rootfs.img" "$R" || { echo EXTRA_FAIL; exit 1; }
echo "=== $NAME in extra-rootfs ==="
od -A d -t x1 -N 64 "$R/usr/lib/$NAME"
echo "=== readelf ==="
readelf -h "$R/usr/lib/$NAME" 2>&1 | grep -E 'Class|Type|Entry|headers|Number of section' || true
umount "$R"; rmdir "$R"
umount "$M"; rmdir "$M"
losetup -d "$L"