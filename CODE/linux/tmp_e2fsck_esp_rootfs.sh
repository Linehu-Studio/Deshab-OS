#!/usr/bin/env bash
# e2fsck the Arch rootfs that QEMU actually boots (native GPT ESP copy).
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/e2fsck_esp_rootfs.out
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true
IMG=/home/deshab/deshab-dev.img
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${LOOP}p1" "$M"
RF="$M/boot/linux-rootfs.img"
ls -lh "$RF"
echo "===== e2fsck -fn ====="
e2fsck -fn "$RF" || true
echo "===== inodes ====="
for ino in 1673 1810 1826; do
    echo "--- inode $ino ---"
    debugfs -R "stat <$ino>" "$RF" 2>/dev/null | head -n 16 || true
    debugfs -R "ncheck $ino" "$RF" 2>/dev/null || true
done
echo "===== /bin /usr/bin mount ====="
debugfs -R 'stat /bin/mount' "$RF" 2>/dev/null | head -n 10 || true
debugfs -R 'stat /usr/bin/mount' "$RF" 2>/dev/null | head -n 10 || true
debugfs -R 'stat /usr/lib/libmount.so.1' "$RF" 2>/dev/null | head -n 10 || true
debugfs -R 'ls -l /lib64' "$RF" 2>/dev/null | head -n 8 || true
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo DONE
