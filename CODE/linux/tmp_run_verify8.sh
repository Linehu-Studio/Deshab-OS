#!/usr/bin/env bash
# Copy the GPT image off drvfs, then verify. Cold-cache Limine loads of
# linux-extra-rootfs.img from /mnt/d OOM/kill QEMU in ~40s on a 7.6G WSL.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify8.out"
NATIVE=/home/deshab/deshab-dev.img
SRC="$ROOT/ISO/deshab-dev.img"
export PYTHONUNBUFFERED=1
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
export IMG_OVERRIDE="$NATIVE"
{
  echo "[run8] $(date -Iseconds)"
  echo "[run8] mem:"; free -h
  echo "[run8] home:"; df -h /home/deshab
  killall qemu-system-x86_64 2>/dev/null || true
  sleep 1
  echo "[run8] copying image to native ext4"
  mkdir -p /home/deshab
  rm -f "$NATIVE"
  cp -f "$SRC" "$NATIVE"
  ls -lh "$NATIVE"
  echo "[run8] starting verify IMG_OVERRIDE=$NATIVE"
} >>"$OUT" 2>&1
sed -i 's/\r$//' "$ROOT/CODE/linux/tmp_reverify2.sh" "$ROOT/CODE/linux/qemu_verify_loop.sh"
# GNU timeout: duration first. Avoid --kill-after (busybox treats it as 15s).
/usr/bin/timeout 540 stdbuf -oL -eL bash "$ROOT/CODE/linux/tmp_reverify2.sh" >>"$OUT" 2>&1
rc=$?
echo "[run8] timed_exit=$rc $(date -Iseconds)" >>"$OUT"
killall qemu-system-x86_64 2>/dev/null || true
exit $rc
