#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
sed -i 's/\r$//' "$ROOT/CODE/linux/qemu_verify_loop.sh"
killall qemu-system-x86_64 2>/dev/null || true
sleep 1
bash "$ROOT/CODE/linux/qemu_verify_loop.sh"
rc=$?
mkdir -p "$ROOT/.build_tmp"
{
    echo "verify_exit=$rc"
    echo "===== markers ====="
    tr -d '\r\0' < "$QEMU_SERIAL_LOG" | grep -E 'EXEC_READY|guest parked|session=|desktop ready|serial 2|deshab-kde|vdc|persist|KDE |scanout|starting Plasma|entering KDE|async spawn|execv failed|VERIFY_' | tail -n 160
} > "$ROOT/.build_tmp/verify_timeslice.txt"
exit $rc
