#!/usr/bin/env bash
# e2fsck Arch rootfs on the GPT ESP. Do not touch extra-rootfs from drvfs.
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
fsck_one() {
    local img="$1"
    [[ -f "$img" ]] || return 0
    echo "[fsck] $img"
    e2fsck -fy "$img" || true
}
fsck_one "$ROOT/SYSTEM/boot/linux-rootfs.img"
for gpt in "$ROOT/ISO/deshab-dev.img" /home/deshab/deshab-dev.img; do
    [[ -f "$gpt" ]] || continue
    loop=$(losetup --find --show --partscan "$gpt")
    sleep 0.3
    m=$(mktemp -d)
    mount "${loop}p1" "$m"
    fsck_one "$m/boot/linux-rootfs.img"
    umount "$m"
    losetup -d "$loop"
    rmdir "$m"
    echo "[fsck] done $gpt"
done
