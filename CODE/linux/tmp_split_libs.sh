#!/usr/bin/env bash
# Split the overlay: recover the PROVEN 140-file xorg-libs set from the
# release image's untouched rootfs, move Agent-2's additions to plasma-libs.
set -Eeuo pipefail
REL=/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-release.img
L=$(losetup --find --show --partscan "$REL")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M"
R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-rootfs.img" "$R"
SRC="$R/usr/local/lib/xorg-libs"
echo "proven set entries: $(ls "$SRC" | wc -l)"
mkdir -p /home/deshab/xorg-libs-proven /home/deshab/plasma-libs
rm -rf /home/deshab/xorg-libs-proven/* /home/deshab/plasma-libs/*
cp -a "$SRC"/. /home/deshab/xorg-libs-proven/
umount "$R"; rmdir "$R"; umount "$M"; rmdir "$M"; losetup -d "$L"

# Move anything NOT in the proven set out of xorg-libs into plasma-libs.
cd /home/deshab/xorg-libs
moved=0
for f in *; do
    if [ ! -e "/home/deshab/xorg-libs-proven/$f" ]; then
        mv -f "$f" /home/deshab/plasma-libs/
        moved=$((moved + 1))
    fi
done
# Proven set wins for any overlap: force-copy it over the remainder.
cp -a /home/deshab/xorg-libs-proven/. /home/deshab/xorg-libs/
echo "moved_to_plasma=$moved"
echo "xorg-libs now: $(ls /home/deshab/xorg-libs | wc -l)"
echo "plasma-libs now: $(ls /home/deshab/plasma-libs | wc -l)"
# Health check the split dirs (non-ELF leftovers allowed only for ld scripts).
bad=0
for d in /home/deshab/xorg-libs /home/deshab/plasma-libs; do
    for f in "$d"/*; do
        [ -L "$f" ] && continue
        if [ -f "$f" ] && [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" != "7f454c46" ]; then
            bad=$((bad + 1)); echo "NONELF: $f"
        fi
    done
done
echo "non_elf_regular=$bad (linker scripts expected: lib*.so style names)"