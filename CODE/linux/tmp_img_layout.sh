#!/usr/bin/env bash
# Report partition layout + filesystem types of a GPT image, without mounting.
set -uo pipefail
img="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
echo "=== image $img ($(stat -c %s "$img") bytes)"
if command -v sgdisk >/dev/null 2>&1; then
    sgdisk -p "$img" 2>/dev/null
else
    echo "(sgdisk missing)"
fi
L=$(losetup --find --show --partscan "$img") || exit 1
sleep 0.3
echo "=== partitions"
lsblk -o NAME,SIZE,FSTYPE,LABEL,UUID "${L}"
for p in "${L}"p*; do
    echo "--- $p"
    file -s "$p" | cut -c1-200
done
losetup -d "$L"