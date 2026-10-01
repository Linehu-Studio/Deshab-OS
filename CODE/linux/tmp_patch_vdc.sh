#!/usr/bin/env bash
# Patch utsm.elf + limine.conf onto GPT images. No QEMU, no extra-rootfs mount.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_patch_vdc.out"
exec > >(stdbuf -oL tee "$OUT") 2>&1
sed -i 's/\r$//' "$ROOT/CODE/linux/patch_boot_on_img.sh"
ls -lh "$ROOT/SYSTEM/boot/utsm.elf" "$ROOT/SYSTEM/limine/limine.conf"
bash "$ROOT/CODE/linux/patch_boot_on_img.sh" "$ROOT/ISO/deshab-dev.img"
echo "[patch] iso_exit=$?"
if [[ -f /home/deshab/deshab-dev.img ]]; then
    bash "$ROOT/CODE/linux/patch_boot_on_img.sh" /home/deshab/deshab-dev.img
    echo "[patch] native_exit=$?"
fi
echo "[patch] done $(date -Iseconds)"
