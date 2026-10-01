#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
killall qemu-system-x86_64 2>/dev/null || true
sleep 1
sed -i 's/\r$//' "$ROOT/CODE/linux/patch_boot_on_img.sh" \
    "$ROOT/CODE/linux/qemu_verify_loop.sh" \
    "$ROOT/CODE/linux/tmp_verify_timeslice.sh"
bash "$ROOT/CODE/linux/patch_boot_on_img.sh"
bash "$ROOT/CODE/linux/tmp_verify_timeslice.sh"
