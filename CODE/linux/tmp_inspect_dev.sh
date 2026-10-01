#!/usr/bin/env bash
# Inspect Arch rootfs + extra-rootfs /dev and mount binaries.
# Never loop-mount extra-rootfs from /mnt/d.
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/inspect_dev.out
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true

echo "===== arch rootfs on SYSTEM (768M, ok to loop) ====="
ARCH=/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/linux-rootfs.img
if [[ -f "$ARCH" ]]; then
    r=$(mktemp -d)
    if mount -o loop,ro "$ARCH" "$r"; then
        echo "[arch] ls -ld $r/dev"
        ls -ld "$r/dev" "$r/bin/mount" "$r/usr/bin/mount" "$r/sbin/mount" "$r/usr/sbin/mount" 2>&1 || true
        echo "[arch] mount candidates"
        ls -l "$r/bin/mount" "$r/usr/bin/mount" "$r/bin/busybox"* 2>&1 || true
        echo "[arch] PATH bins: mount umount chroot"
        for n in mount umount chroot bash; do
            find "$r/bin" "$r/usr/bin" "$r/sbin" "$r/usr/sbin" -name "$n" 2>/dev/null | head
        done
        echo "[arch] /dev contents"
        ls -la "$r/dev" | head -n 40
        umount "$r"
    fi
    rmdir "$r"
fi

echo "===== extra-rootfs via native GPT ESP (debugfs, no drvfs 4G loop) ====="
IMG=/home/deshab/deshab-dev.img
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${LOOP}p1" "$M"
EXTRA="$M/boot/linux-extra-rootfs.img"
ls -lh "$EXTRA" "$M/boot/linux-rootfs.img" || true
if [[ -f "$EXTRA" ]]; then
    echo "[extra] debugfs ls /"
    debugfs -R 'ls -l /' "$EXTRA" 2>/dev/null | head -n 40 || true
    echo "[extra] debugfs stat /dev"
    debugfs -R 'stat /dev' "$EXTRA" 2>/dev/null | head -n 25 || true
    echo "[extra] debugfs ls /dev"
    debugfs -R 'ls -l /dev' "$EXTRA" 2>/dev/null | head -n 40 || true
    echo "[extra] debugfs stat /usr/bin/mount"
    debugfs -R 'stat /usr/bin/mount' "$EXTRA" 2>/dev/null | head -n 15 || true
    echo "[extra] debugfs stat /usr/bin/Xorg"
    debugfs -R 'stat /usr/bin/Xorg' "$EXTRA" 2>/dev/null | head -n 12 || true
    echo "[extra] debugfs stat /usr/lib/xorg/modules/drivers/modesetting_drv.so"
    debugfs -R 'stat /usr/lib/xorg/modules/drivers/modesetting_drv.so' "$EXTRA" 2>/dev/null | head -n 12 || true
fi
if [[ -f "$M/boot/linux-rootfs.img" ]]; then
    echo "[esp-arch] debugfs stat /usr/bin/mount"
    debugfs -R 'stat /usr/bin/mount' "$M/boot/linux-rootfs.img" 2>/dev/null | head -n 12 || true
    echo "[esp-arch] debugfs ls /bin"
    debugfs -R 'ls -l /bin' "$M/boot/linux-rootfs.img" 2>/dev/null | tr ' ' '\n' | grep -E 'mount|busybox|bash' || true
fi
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo DONE
