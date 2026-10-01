#!/usr/bin/env bash
# Report ESP payload identity for GPT images: md5 + size + build markers.
# Usage: tmp_img_esp_report.sh IMG [IMG...]
set -uo pipefail

FILES="boot/utsm.elf boot/linux-bzImage boot/linux-initrd.img limine.conf system/deshab64/FUCK"

for img in "$@"; do
    if [[ ! -f "$img" ]]; then
        echo "$(basename "$img"): missing"
        continue
    fi
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    if mount "${L}p1" "$M" 2>/dev/null; then
        echo "== $(basename "$img")  hdr_md5=$(dd if="$img" bs=1M count=2 status=none | md5sum | cut -d' ' -f1)"
        for f in $FILES; do
            if [[ -f "$M/$f" ]]; then
                printf '   %-26s size=%-10s md5=%s\n' \
                    "$f" "$(stat -c %s "$M/$f")" "$(md5sum "$M/$f" | cut -d' ' -f1)"
            else
                printf '   %-26s MISSING\n' "$f"
            fi
        done
        if [[ -f "$M/boot/utsm.elf" ]]; then
            printf '   %-26s FLUSH_N=%s\n' "boot/utsm.elf" \
                "$(strings -a "$M/boot/utsm.elf" | grep -c FLUSH_N || true)"
        fi
        umount "$M"
    else
        echo "$(basename "$img"): mount p1 failed"
    fi
    rmdir "$M"
    losetup -d "$L"
done