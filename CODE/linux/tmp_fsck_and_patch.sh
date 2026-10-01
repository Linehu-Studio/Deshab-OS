#!/usr/bin/env bash
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_fsck_patch.out"
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true
sed -i 's/\r$//' \
  "$ROOT/CODE/linux/tmp_patch_persist.sh" \
  "$ROOT/CODE/linux/rebuild_initrd.sh" \
  "$ROOT/CODE/linux/patch_boot_on_img.sh" \
  "$ROOT/CODE/linux/guest/deshab-kde-session"
echo "[fsck] copy rootfs to native disk"
cp -f "$ROOT/SYSTEM/boot/linux-rootfs.img" /home/deshab/linux-rootfs-fsck.img
ls -lh /home/deshab/linux-rootfs-fsck.img
e2fsck -fy /home/deshab/linux-rootfs-fsck.img || true
cp -f /home/deshab/linux-rootfs-fsck.img "$ROOT/SYSTEM/boot/linux-rootfs.img"
echo "[fsck] copied back"
bash "$ROOT/CODE/linux/tmp_patch_persist.sh"
echo "[fsck] ALL_DONE $(date -Iseconds)"
