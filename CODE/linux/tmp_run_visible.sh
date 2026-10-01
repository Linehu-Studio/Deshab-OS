#!/usr/bin/env bash
# Visible KDE verify for Windows Terminal (not Cursor).
# Copies the GPT image off drvfs first. extra-rootfs is NOT a Limine module
# (that 4GiB RAM load killed 6G WSL QEMU at log=21013). vdc maps the ESP file.
# Freshness is decided by the ESP /boot/utsm.elf md5, not by file size: two
# builds of the same variant are byte-similar in length but differ in payload,
# and a size-only gate silently reuses a stale kernel.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify_visible.out"
NATIVE=/home/deshab/deshab-dev.img
SRC="$ROOT/ISO/deshab-dev.img"
export PYTHONUNBUFFERED=1
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
export IMG_OVERRIDE="$NATIVE"

sed -i 's/\r$//' \
  "$ROOT/CODE/linux/tmp_reverify2.sh" \
  "$ROOT/CODE/linux/qemu_verify_loop.sh" \
  "$ROOT/ISO/run_qemu_kvm.sh" || true

echo "[visible] $(date -Iseconds)  log also: $OUT"
killall qemu-system-x86_64 2>/dev/null || true
sleep 1

esp_utsm_md5() {
    local img="$1" L M out=""
    [[ -f "$img" ]] || { echo ""; return 0; }
    L=$(losetup --find --show --partscan "$img" 2>/dev/null) || { echo ""; return 0; }
    sleep 0.3
    M=$(mktemp -d)
    if mount "${L}p1" "$M" 2>/dev/null; then
        out=$(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1)
        umount "$M" 2>/dev/null || true
    fi
    rmdir "$M" 2>/dev/null || true
    losetup -d "$L" 2>/dev/null || true
    echo "$out"
}

want=$(esp_utsm_md5 "$SRC")
have=$(esp_utsm_md5 "$NATIVE")
echo "[visible] utsm.elf md5 want=${want:-READ_FAIL} native=${have:-none}"
if [[ -z "$want" ]]; then
    echo "[visible] refusing to run: cannot read /boot/utsm.elf from $SRC"
    exit 1
fi

if [[ "$have" == "$want" ]]; then
    echo "[visible] native image payload is current"
    ls -lh "$NATIVE"
elif [[ -n "$have" ]]; then
    echo "[visible] native image payload stale -> re-syncing ESP in place"
    bash "$ROOT/CODE/linux/patch_boot_on_img.sh" "$NATIVE" "$ROOT"
    have=$(esp_utsm_md5 "$NATIVE")
    [[ "$have" == "$want" ]] || { echo "[visible] re-sync failed (native=${have:-none})"; exit 1; }
    echo "[visible] native image payload now current"
else
    echo "[visible] native image missing/unreadable, copying to WSL ext4 (several minutes)..."
    rm -f "$NATIVE"
    mkdir -p /home/deshab
    dd if="$SRC" of="$NATIVE" bs=64M status=progress conv=fsync
    ls -lh "$NATIVE"
fi

echo "[visible] starting QEMU (heartbeat every 2s). extra-rootfs is on-disk, not a Limine module."
set -o pipefail
bash "$ROOT/CODE/linux/tmp_reverify2.sh" 2>&1 | tee -a "$OUT"
rc=${PIPESTATUS[0]}
echo "[visible] verify_exit=$rc $(date -Iseconds)"
exit $rc
