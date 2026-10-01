#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/timeslice_build.log"
mkdir -p "$ROOT/.build_tmp"
exec > >(tee "$OUT") 2>&1

echo "===== extra-rootfs debugfs ====="
for img in /home/deshab/linux-extra-rootfs.img /root/extra_rootfs_work/extra-rootfs.img; do
    echo "-- $img --"
    if [[ -f "$img" ]]; then
        debugfs -R 'stat /usr/bin/startplasma-x11' "$img" || true
        debugfs -R 'ls /usr/bin' "$img" 2>/dev/null | tr ' ' '\n' | grep -E 'startplasma|plasmashell|Xorg' || true
    fi
done

echo "===== kill qemu ====="
killall qemu-system-x86_64 2>/dev/null || true
sleep 1

echo "===== rebuild initrd ====="
sed -i 's/\r$//' "$ROOT/CODE/linux/rebuild_initrd.sh" \
    "$ROOT/CODE/linux/inject_boot_and_busybox.sh" \
    "$ROOT/CODE/linux/patch_boot_on_img.sh" \
    "$ROOT/CODE/linux/qemu_verify_loop.sh" \
    "$ROOT/CODE/linux/guest/deshab-kde-session"
bash "$ROOT/CODE/linux/rebuild_initrd.sh"
bash "$ROOT/CODE/linux/inject_boot_and_busybox.sh"
echo BUILD_INJECT_OK
