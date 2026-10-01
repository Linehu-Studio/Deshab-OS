#!/usr/bin/env bash
set -u
cd /mnt/d/Code/DEAICUP/Deshab
echo "SYSTEM: $(md5sum SYSTEM/boot/utsm.elf | cut -d' ' -f1)"
for img in ISO/deshab-dev.img ISO/deshab-release.img ISO/deshab-realtest.img; do
    L=$(losetup --find --show --partscan "$img" 2>/dev/null) || { echo "$img: LOSETUP_FAIL"; continue; }
    sleep 0.3
    M=$(mktemp -d)
    if mount "${L}p1" "$M" 2>/dev/null; then
        echo "$img: $(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)"
        umount "$M" 2>/dev/null
    else
        echo "$img: MOUNT_FAIL"
    fi
    rmdir "$M" 2>/dev/null
    losetup -d "$L" 2>/dev/null
done
