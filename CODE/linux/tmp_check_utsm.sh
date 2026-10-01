#!/usr/bin/env bash
# Report which GPT images already carry the current SYSTEM/boot/utsm.elf.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
EXP=$(md5sum "$ROOT/SYSTEM/boot/utsm.elf" | cut -d' ' -f1)
echo "expected utsm.elf md5=$EXP size=$(stat -c %s "$ROOT/SYSTEM/boot/utsm.elf")"
for img in "$@"; do
    [[ -f "$img" ]] || { echo "$img: missing"; continue; }
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    if mount "${L}p1" "$M" 2>/dev/null; then
        got=$(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)
        sz=$(stat -c %s "$M/boot/utsm.elf" 2>/dev/null || echo 0)
        n=$(strings -a "$M/boot/utsm.elf" 2>/dev/null | grep -c FLUSH_N || true)
        if [[ "$got" == "$EXP" ]]; then st=CURRENT; else st=STALE; fi
        echo "$(basename "$img"): $st sz=$sz FLUSH_N=$n"
        umount "$M"
    else
        echo "$(basename "$img"): mount p1 failed"
    fi
    rmdir "$M"
    losetup -d "$L"
done