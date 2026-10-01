#!/usr/bin/env bash
# Re-sync the ESP boot payload (+ guest scripts / X11 libs) on existing GPT
# images without recopying the 4GiB extra-rootfs. Then prove the in-image
# utsm.elf md5 matches SYSTEM/boot/utsm.elf.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
expected=$(md5sum "$ROOT/SYSTEM/boot/utsm.elf" | cut -d' ' -f1)
echo "[patch-all] expected utsm.elf md5=$expected"

for img in "$@"; do
    [[ -f "$img" ]] || { echo "[patch-all] $img: missing"; continue; }
    echo "[patch-all] === $(basename "$img") ==="
    bash "$ROOT/CODE/linux/patch_boot_on_img.sh" "$img" "$ROOT" || {
        echo "[patch-all] $img: PATCH FAILED"
        continue
    }
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    if mount "${L}p1" "$M" 2>/dev/null; then
        got=$(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)
        n=$(strings -a "$M/boot/utsm.elf" 2>/dev/null | grep -c FLUSH_N || true)
        umount "$M"
        if [[ "$got" == "$expected" ]]; then
            echo "[patch-all] $(basename "$img"): CURRENT utsm.elf md5=$got FLUSH_N=$n"
        else
            echo "[patch-all] $(basename "$img"): STILL STALE md5=$got"
        fi
    else
        echo "[patch-all] $(basename "$img"): mount p1 failed"
    fi
    rmdir "$M"
    losetup -d "$L"
done