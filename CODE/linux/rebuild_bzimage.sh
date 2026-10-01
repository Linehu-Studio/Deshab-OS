#!/usr/bin/env bash
# Incremental rebuild of linux-bzImage after UTSM driver patches.
# Linux sources live on the WSL native disk; never edit them via /mnt/d.
set -euo pipefail

LINUX_SRC="${LINUX_SRC:-/home/deshab/linux-6.6}"
REPO="${REPO:-/mnt/d/Code/DEAICUP/Deshab}"
PATCH="$REPO/CODE/linux/patches/utsm_hcall.c"
OUT="$REPO/SYSTEM/boot/linux-bzImage"

if [[ ! -f "$LINUX_SRC/Makefile" ]]; then
    echo "[build] missing $LINUX_SRC" >&2
    exit 1
fi
if [[ ! -f "$PATCH" ]]; then
    echo "[build] missing $PATCH" >&2
    exit 1
fi

cp "$PATCH" "$LINUX_SRC/drivers/utsm/utsm_hcall.c"
if id deshab >/dev/null 2>&1; then
    chown deshab:deshab "$LINUX_SRC/drivers/utsm/utsm_hcall.c" || true
fi

echo "[build] installed driver from $PATCH"
grep -n 'memcpy(umsg.data' "$LINUX_SRC/drivers/utsm/utsm_hcall.c"
grep -n 'memcpy(msg.data' "$LINUX_SRC/drivers/utsm/utsm_hcall.c"

cd "$LINUX_SRC"
echo "[build] incremental bzImage start $(date -Iseconds)"
make -j"$(nproc)" KCFLAGS="-std=gnu17" bzImage
cp -f arch/x86/boot/bzImage "$OUT"
ls -la arch/x86/boot/bzImage "$OUT"
echo "[build] bzImage done $(date -Iseconds)"
