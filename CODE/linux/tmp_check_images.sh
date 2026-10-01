#!/usr/bin/env bash
# Report the ESP /boot/utsm.elf md5 of every GPT image plus a few key ESP
# payload files, so we know which images carry the FLUSH_N kernel.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
SRC="$ROOT/SYSTEM/boot/utsm.elf"
WANT=$(md5sum "$SRC" | cut -d' ' -f1)

echo "want utsm.elf md5=$WANT size=$(stat -c %s "$SRC")"

for img in "$@"; do
    [[ -f "$img" ]] || { echo "$(basename "$img"): MISSING"; continue; }
    L=$(losetup --find --show --partscan "$img" 2>/dev/null)
    if [[ -z "$L" ]]; then echo "$(basename "$img"): losetup failed"; continue; fi
    sleep 0.3
    M=$(mktemp -d)
    if ! mount "${L}p1" "$M" 2>/dev/null; then
        echo "$(basename "$img"): mount p1 failed"
    else
        got=$(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)
        if [[ "$got" == "$WANT" ]]; then verdict=UP-TO-DATE; else verdict=STALE; fi
        echo "$(basename "$img"): utsm=${got:-none} $verdict"
        for f in boot/linux-rootfs.img boot/linux-extra-rootfs.img \
                 boot/vmlinuz-linux boot/initramfs-linux.img; do
            if [[ -f "$M/$f" ]]; then
                printf '    %-28s %12s %s\n' "$f" "$(stat -c %s "$M/$f")" "$(date -r "$M/$f" '+%m-%d %H:%M')"
            else
                printf '    %-28s %12s\n' "$f" absent
            fi
        done
        umount "$M"
    fi
    rmdir "$M" 2>/dev/null
    losetup -d "$L" 2>/dev/null
done