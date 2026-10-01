#!/usr/bin/env bash
# Rebuild initrd (persist bind) and patch both GPT images. No QEMU.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_patch_persist.out"
exec > >(stdbuf -oL tee "$OUT") 2>&1
sed -i 's/\r$//' \
  "$ROOT/CODE/linux/rebuild_initrd.sh" \
  "$ROOT/CODE/linux/patch_boot_on_img.sh" \
  "$ROOT/CODE/linux/initramfs/init" \
  "$ROOT/CODE/linux/guest/deshab-kde-session"
export SKIP_ROOTFS_PATCH=1
bash "$ROOT/CODE/linux/rebuild_initrd.sh"
rc=$?
echo "[patch] initrd_exit=$rc"
[[ $rc -eq 0 ]] || exit $rc
bash "$ROOT/CODE/linux/patch_boot_on_img.sh" "$ROOT/ISO/deshab-dev.img"
echo "[patch] iso_exit=$?"
if [[ -f /home/deshab/deshab-dev.img ]]; then
    bash "$ROOT/CODE/linux/patch_boot_on_img.sh" /home/deshab/deshab-dev.img
    echo "[patch] native_exit=$?"
fi
echo "[patch] done $(date -Iseconds)"
