#!/usr/bin/env bash
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify6.out"
exec >"$OUT" 2>&1
sed -i 's/\r$//' \
  "$ROOT/CODE/linux/rebuild_initrd.sh" \
  "$ROOT/CODE/linux/patch_boot_on_img.sh" \
  "$ROOT/CODE/linux/tmp_reverify2.sh" \
  "$ROOT/CODE/linux/qemu_verify_loop.sh" \
  "$ROOT/CODE/linux/initramfs/init" \
  "$ROOT/CODE/linux/initramfs/bin/utsm_exec_daemon.c" \
  "$ROOT/CODE/linux/guest/deshab-kde-session" || true
killall qemu-system-x86_64 2>/dev/null || true
sleep 2
export SKIP_ROOTFS_PATCH=1
bash "$ROOT/CODE/linux/rebuild_initrd.sh"
rc_initrd=$?
echo "[run] initrd_exit=$rc_initrd"
if [[ $rc_initrd -ne 0 ]]; then
    exit $rc_initrd
fi
bash "$ROOT/CODE/linux/patch_boot_on_img.sh"
rc_patch=$?
echo "[run] patch_exit=$rc_patch"
if [[ $rc_patch -ne 0 ]]; then
    exit $rc_patch
fi
bash "$ROOT/CODE/linux/tmp_reverify2.sh"
exit $?
