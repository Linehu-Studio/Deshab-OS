#!/usr/bin/env bash
# Deep health scan of the ESP linux-rootfs.img X11 payload:
# resolves symlinks, reports ELF validity, .text size and dynamic symbols so a
# "smashed header" library (right size, truncated .text) is obvious.
set -uo pipefail
img="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
dir="${2:-/usr/local/lib/xorg-libs}"
L=$(losetup --find --show --partscan "$img"); sleep 0.3
M=$(mktemp -d); mount "${L}p1" "$M" || { echo "ESP mount failed"; exit 1; }
R=$(mktemp -d); mount -o loop,ro "$M/boot/linux-rootfs.img" "$R" || { echo "rootfs mount failed"; exit 1; }
D="$R$dir"
echo "=== $img : $dir ($(ls "$D" 2>/dev/null | wc -l) entries)"
printf '%-34s %-9s %9s %9s %7s %s\n' NAME KIND SIZE TEXT SYMS NOTE
while IFS= read -r f; do
    name=$(basename "$f")
    if [[ -L "$f" ]]; then
        tgt=$(readlink -f "$f" 2>/dev/null || true)
        if [[ -z "$tgt" || ! -e "$tgt" ]]; then
            printf '%-34s %-9s %9s %9s %7s %s\n' "$name" SYMLINK - - - "DANGLING -> $(readlink "$f")"
            continue
        fi
        f="$tgt"
    fi
    magic=$(head -c4 "$f" 2>/dev/null | tr -d '\0')
    if [[ "$magic" != $'\x7fELF' ]]; then
        printf '%-34s %-9s %9s %9s %7s %s\n' "$name" NOT-ELF "$(stat -c %s "$f")" - - "$(file -b "$f" | cut -c1-30)"
        continue
    fi
    sz=$(stat -c %s "$f")
    text=$(readelf -SW "$f" 2>/dev/null | awk '$2==".text"{print strtonum("0x"$6)}')
    syms=$(readelf -sW --dyn-syms "$f" 2>/dev/null | grep -c 'FUNC')
    note=""
    [[ -z "$text" ]] && { note="NO-.text"; text=0; }
    [[ "$text" -lt 4096 && "$sz" -gt 20000 ]] && note="${note} SMALL-TEXT"
    [[ "$syms" -lt 5 ]] && note="${note} FEW-SYMS"
    printf '%-34s %-9s %9s %9s %7s %s\n' "$name" ELF "$sz" "$text" "$syms" "$note"
done < <(find "$D" -maxdepth 1 \( -type f -o -type l \) | sort)
umount "$R"; rmdir "$R"; umount "$M"; losetup -d "$L"; rmdir "$M"