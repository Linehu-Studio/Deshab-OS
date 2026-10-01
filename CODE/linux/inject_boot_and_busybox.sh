#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
BB="$ROOT/CODE/linux/.cache/busybox"
mkdir -p "$ROOT/CODE/linux/.cache"
if [ ! -x "$BB" ]; then
    echo "[bb] downloading busybox"
    if ! wget -q -O "$BB" \
        https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox; then
        echo "[bb] extract from initrd"
        XT=$(mktemp -d)
        (cd "$XT" && cpio -id < "$ROOT/SYSTEM/boot/linux-initrd.img" >/dev/null 2>&1) || true
        if [ -x "$XT/bin/busybox" ]; then
            cp "$XT/bin/busybox" "$BB"
        fi
        rm -rf "$XT"
    fi
    chmod +x "$BB"
fi
ls -l "$BB"
sed -i 's/\r$//' \
    "$ROOT/CODE/linux/patch_boot_on_img.sh" \
    "$ROOT/CODE/linux/guest/deshab-kde-session"
bash "$ROOT/CODE/linux/patch_boot_on_img.sh"
echo PATCH_BOOT_OK
