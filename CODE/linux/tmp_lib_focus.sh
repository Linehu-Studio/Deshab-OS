#!/usr/bin/env bash
# Focused health check: does the ESP rootfs payload contain a *loadable* copy of
# every X11 lib the KDE session depends on? Reports name, kind, size, .text.
set -uo pipefail
img="${1:?usage: tmp_lib_focus.sh IMG}"
dir=/usr/local/lib/xorg-libs
L=$(losetup --find --show --partscan "$img"); sleep 0.3
M=$(mktemp -d); mount "${L}p1" "$M" || { echo "ESP mount failed"; exit 1; }
echo "=== $(basename "$img")"
echo "utsm.elf md5=$(md5sum "$M/boot/utsm.elf" | cut -d' ' -f1)"
R=$(mktemp -d); mount -o loop,ro "$M/boot/linux-rootfs.img" "$R" || { echo "rootfs mount failed"; exit 1; }
D="$R$dir"
echo "--- $dir ($(ls "$D" 2>/dev/null | wc -l) entries) ---"
printf '%-30s %-9s %10s %9s %6s %s\n' NAME KIND SIZE TEXT SYMS NOTE
for n in libinput.so.10 libffi.so.8 libXdmcp.so.6 libXau.so.6 libdrm.so.2 \
         libxfont2.so.2 libpixman-1.so.0 libevdev.so.2 libmtdev.so.1 \
         libpciaccess.so.0 libfontenc.so.1 libxshmfence.so.1 libxcvt.so.0 \
         libXfont.so.1 libwayland-client.so.0 libepoxy.so.0; do
    f="$D/$n"
    if [[ ! -e "$f" ]]; then printf '%-30s %-9s\n' "$n" MISSING; continue; fi
    if [[ -L "$f" ]]; then t=$(readlink -f "$f"); k=SYMLINK; else t="$f"; k=FILE; fi
    if [[ ! -e "$t" ]]; then printf '%-30s %-9s %s\n' "$n" DANGLING "$(readlink "$f")"; continue; fi
    magic=$(head -c4 "$t" | tr -d '\0')
    sz=$(stat -c %s "$t")
    if [[ "$magic" != $'\x7fELF' ]]; then
        printf '%-30s %-9s %10s %9s %6s %s\n' "$n" NOT-ELF "$sz" - - "$(file -b "$t" | cut -c1-28)"
        continue
    fi
    text=$(readelf -SW "$t" 2>/dev/null | awk '$2==".text"{print strtonum("0x"$6)}')
    syms=$(readelf -sW --dyn-syms "$t" 2>/dev/null | grep -c FUNC)
    note=""
    [[ -z "$text" ]] && { note="NO-.text"; text=0; }
    [[ "$text" -lt 4096 && "$sz" -gt 20000 ]] && note="$note SMALL-TEXT"
    [[ "$syms" -lt 5 ]] && note="$note FEW-SYMS"
    printf '%-30s %-9s %10s %9s %6s %s\n' "$n" "$k" "$sz" "$text" "$syms" "$note"
done
echo "--- Xorg / input modules ---"
for p in usr/local/lib/Xorg.good usr/local/lib/Xorg usr/lib/Xorg \
         usr/local/lib/xorg/modules/drivers/modesetting_drv.so \
         usr/local/lib/xorg/modules/input/libinput_drv.so \
         usr/local/lib/xorg/modules/input/mouse_drv.so \
         usr/local/lib/xorg/modules/input/kbd_drv.so; do
    f="$R/$p"
    if [[ -e "$f" ]]; then
        printf '%-56s %10s %s\n' "$p" "$(stat -c %s "$f")" "$(file -b "$f" | cut -c1-34)"
    else
        printf '%-56s %10s\n' "$p" absent
    fi
done
umount "$R"; rmdir "$R"; umount "$M"; losetup -d "$L"; rmdir "$M"