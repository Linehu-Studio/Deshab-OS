#!/usr/bin/env bash
# Inspect GPT images: embedded utsm.elf, ESP files, and X11 library health
# inside the ESP linux-rootfs.img (/usr/local/lib/xorg-libs + Xorg.good).
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
EXP=$(md5sum "$ROOT/SYSTEM/boot/utsm.elf" | cut -d' ' -f1)
echo "expected utsm.elf md5=$EXP size=$(stat -c %s "$ROOT/SYSTEM/boot/utsm.elf")"

check_lib() {
    local f="$1"
    if [[ ! -e "$f" ]]; then echo "  MISSING $f"; return; fi
    local kind
    kind=$(file -b "$f" 2>/dev/null | cut -c1-60)
    local ent=""
    if [[ "$kind" == ELF* ]]; then
        ent=$(readelf -h "$f" 2>/dev/null | awk '/Entry point/{print $4}')
    fi
    printf '  %-46s %10s  %-40s entry=%s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$kind" "$ent"
}

for img in "$@"; do
    echo "=== $img"
    [[ -f "$img" ]] || { echo "  missing"; continue; }
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    mount "${L}p1" "$M" 2>/dev/null || { echo "  ESP mount failed"; losetup -d "$L"; rmdir "$M"; continue; }
    got=$(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)
    n=$(strings -a "$M/boot/utsm.elf" 2>/dev/null | grep -c FLUSH_N || true)
    [[ "$got" == "$EXP" ]] && st=CURRENT || st=STALE
    echo "  utsm.elf: $st md5=$got FLUSH_N=$n"
    for f in boot/linux-bzImage boot/linux-initrd.img boot/linux-rootfs.img boot/linux-extra-rootfs.img; do
        [[ -f "$M/$f" ]] && echo "  $(basename "$f"): $(stat -c %s "$M/$f") bytes  $(stat -c %y "$M/$f" | cut -c1-19)"
    done
    R=$(mktemp -d)
    if mount -o loop,ro "$M/boot/linux-rootfs.img" "$R" 2>/dev/null; then
        echo "  -- rootfs /usr/local/lib/xorg-libs: $(ls "$R/usr/local/lib/xorg-libs" 2>/dev/null | wc -l) files"
        check_lib "$R/usr/local/lib/Xorg.good"
        for lib in libinput.so.10 libffi.so.8 libXdmcp.so.6.0.0 libpixman-1.so.0 libXau.so.6.0.0 libdrm.so.2 libxfont2.so.2 libXfont2.so.2 libeudev.so.1 libwacom.so.9 libmtdev.so.1 libevdev.so.2 libgudev-1.0.so.0 libxkbcommon.so.0 libxcb.so.1 libX11.so.6; do
            check_lib "$R/usr/local/lib/xorg-libs/$lib"
        done
        echo "  -- rootfs own copies:"
        for p in usr/lib/Xorg usr/lib/libinput.so.10 usr/lib/libffi.so.8 usr/lib/libc.so.6 usr/lib/libXdmcp.so.6.0.0; do
            check_lib "$R/$p"
        done
        echo "  -- xorg modules(drivers): $(ls "$R/usr/local/lib/xorg/modules/drivers" 2>/dev/null | tr '\n' ' ')"
        umount "$R"
    else
        echo "  rootfs mount failed"
    fi
    rmdir "$R"
    umount "$M"
    losetup -d "$L"
    rmdir "$M"
done