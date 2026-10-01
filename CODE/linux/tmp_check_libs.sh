#!/usr/bin/env bash
# Health report for the X11 library payload inside an image's ESP rootfs.
set -uo pipefail
img="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
L=$(losetup --find --show --partscan "$img"); sleep 0.3
M=$(mktemp -d); mount "${L}p1" "$M"
echo "--- ESP entry mtimes (dev image uses current build time) ---"
find "$M/boot" "$M/system/deshab64" -maxdepth 1 -type f -printf '%TY-%Tm-%Td %TH:%TM %10s %p\n' 2>/dev/null | sort
R=$(mktemp -d); mount -o loop,ro "$M/boot/linux-rootfs.img" "$R"
D="$R/usr/local/lib/xorg-libs"
echo "--- xorg-libs: $(ls "$D" | wc -l) entries ---"
bad=0; good=0
while IFS= read -r f; do
    if [[ -L "$f" ]]; then
        tgt=$(readlink -f "$f" 2>/dev/null)
        if [[ -z "$tgt" || ! -e "$tgt" ]]; then echo "  DANGLING $(basename "$f") -> $(readlink "$f")"; bad=$((bad+1)); continue; fi
        f="$tgt"
    fi
    if head -c4 "$f" 2>/dev/null | grep -q ELF; then
        good=$((good+1))
    else
        echo "  NOT-ELF  $(basename "$f")  $(stat -c %s "$f") bytes  $(file -b "$f" | cut -c1-40)"
        bad=$((bad+1))
    fi
done < <(find "$D" -maxdepth 1 -type f -o -maxdepth 1 -type l | sort)
echo "  elf_ok=$good bad=$bad"
echo "--- non-lib extras in /usr/local/lib ---"
ls -l "$R/usr/local/lib" | grep -v '^d' | head -20
echo "--- /usr/local/bin ---"
ls -l "$R/usr/local/bin" 2>/dev/null
umount "$R"; rmdir "$R"; umount "$M"; losetup -d "$L"; rmdir "$M"