#!/usr/bin/env bash
# Hard-capped verify. Do not use `exec >file` (block-buffered, hides SUMMARY).
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/wsl_verify7.out"
export PYTHONUNBUFFERED=1
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
echo "[run7] $(date -Iseconds) kill leftover qemu" >"$OUT"
killall qemu-system-x86_64 2>/dev/null || true
sleep 2
ss -ltnp | grep -E '45454|45455' >>"$OUT" 2>&1 || echo "[run7] ports free" >>"$OUT"
sed -i 's/\r$//' "$ROOT/CODE/linux/tmp_reverify2.sh" "$ROOT/CODE/linux/qemu_verify_loop.sh"
echo "[run7] starting timed verify" >>"$OUT"
timeout --kill-after=15 540 stdbuf -oL -eL bash "$ROOT/CODE/linux/tmp_reverify2.sh" >>"$OUT" 2>&1
rc=$?
echo "[run7] timed_exit=$rc $(date -Iseconds)" >>"$OUT"
killall qemu-system-x86_64 2>/dev/null || true
exit $rc
