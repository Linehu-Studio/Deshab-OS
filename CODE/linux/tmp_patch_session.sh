#!/usr/bin/env bash
# Inject updated KDE session + verify script bits. No initrd rebuild.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_patch_session.out"
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true
sed -i 's/\r$//' \
  "$ROOT/CODE/linux/patch_boot_on_img.sh" \
  "$ROOT/CODE/linux/qemu_verify_loop.sh" \
  "$ROOT/CODE/linux/guest/deshab-kde-session" \
  "$ROOT/CODE/linux/tmp_run_visible.sh"
echo "[patch] compiling utsm_bindmount utsm_run"
cc -static -O2 -Wall -fcf-protection=none \
    -o "$ROOT/.build_tmp/utsm_bindmount" \
    "$ROOT/CODE/linux/initramfs/bin/utsm_bindmount.c"
cc -static -O2 -Wall -fcf-protection=none \
    -o "$ROOT/.build_tmp/utsm_run" \
    "$ROOT/CODE/linux/initramfs/bin/utsm_run.c"
strip "$ROOT/.build_tmp/utsm_bindmount" 2>/dev/null || true
strip "$ROOT/.build_tmp/utsm_run" 2>/dev/null || true
ls -l "$ROOT/.build_tmp/utsm_bindmount" "$ROOT/.build_tmp/utsm_run"
export XORG_LIBS=/home/deshab/xorg-libs
export PLASMA_LIBS=/home/deshab/plasma-libs
export MODESET=/home/deshab/modesetting_drv.so
bash "$ROOT/CODE/linux/patch_boot_on_img.sh" "$ROOT/ISO/deshab-dev.img"
echo "[patch] iso_exit=$?"
if [[ -f /home/deshab/deshab-dev.img ]]; then
    bash "$ROOT/CODE/linux/patch_boot_on_img.sh" /home/deshab/deshab-dev.img
    echo "[patch] native_exit=$?"
fi
echo "[patch] done $(date -Iseconds)"
