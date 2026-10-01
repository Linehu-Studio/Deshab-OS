#!/usr/bin/env bash
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify9.out"
export PYTHONUNBUFFERED=1
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
export IMG_OVERRIDE="$ROOT/ISO/deshab-dev.img"
echo "[run9] $(date -Iseconds) nohup verify on drvfs image" >>"$OUT"
killall qemu-system-x86_64 2>/dev/null || true
sleep 2
sed -i 's/\r$//' "$ROOT/CODE/linux/tmp_reverify2.sh" "$ROOT/CODE/linux/qemu_verify_loop.sh"
/usr/bin/timeout 540 stdbuf -oL -eL bash "$ROOT/CODE/linux/tmp_reverify2.sh" >>"$OUT" 2>&1
rc=$?
echo "[run9] timed_exit=$rc $(date -Iseconds)" >>"$OUT"
killall qemu-system-x86_64 2>/dev/null || true
exit $rc
