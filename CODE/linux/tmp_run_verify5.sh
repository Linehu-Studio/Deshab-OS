#!/usr/bin/env bash
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify5.out"
exec >"$OUT" 2>&1
sed -i 's/\r$//' \
  "$ROOT/CODE/linux/patch_boot_on_img.sh" \
  "$ROOT/CODE/linux/tmp_reverify2.sh" \
  "$ROOT/CODE/linux/guest/deshab-kde-session" \
  "$ROOT/CODE/linux/qemu_verify_loop.sh" || true
killall qemu-system-x86_64 2>/dev/null || true
sleep 2
bash "$ROOT/CODE/linux/patch_boot_on_img.sh"
rc_patch=$?
echo "[run] patch_exit=$rc_patch"
if [[ $rc_patch -ne 0 ]]; then
    exit $rc_patch
fi
bash "$ROOT/CODE/linux/tmp_reverify2.sh"
exit $?
