#!/usr/bin/env bash
set -u
IMG=/home/deshab/deshab-dev.img
L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M"
R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-rootfs.img" "$R"
XL="$R/usr/local/lib/xorg-libs"
echo "== xorg-libs count =="
ls "$XL" | wc -l
echo "== libc/libdbus/ld in overlay =="
ls -la "$XL" | grep -E 'libc|libdbus|ld-linux' || echo "(none in overlay)"
echo "== full listing =="
ls "$XL"
echo "== ELF health scan of overlay =="
bad=0
for f in "$XL"/*; do
    [ -f "$f" ] || continue
    t=$(file -b "$f")
    case "$t" in
        *ELF*) ;;
        *"symbolic link"*) ;;
        *) echo "BAD: $(basename "$f"): $t"; bad=$((bad+1));;
    esac
done
echo "bad=$bad"
echo "== which libc would win: LD_LIBRARY_PATH=/usr/local/lib/xorg-libs =="
ls -la "$XL/libc.so.6" 2>/dev/null || echo "no libc.so.6 in overlay -> persist /usr/lib/libc.so.6 wins"
echo "== persist-side libc check inside vdb rootfs =="
ls -la "$R"/usr/lib/libc.so.6 "$R"/usr/lib/libdbus-1.so.3* 2>/dev/null
file "$R"/usr/lib/libc.so.6 2>/dev/null
umount "$R"; rmdir "$R"
umount "$M"; rmdir "$M"
losetup -d "$L"
