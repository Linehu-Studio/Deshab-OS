#!/usr/bin/env bash
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/wsl_verify_detached.out
LOG=/home/deshab/qemu_serial_loop1.log
echo "===== tail of detached out ====="
tail -n 60 "$OUT"
echo
echo "===== key serial markers ====="
tr -d '\r\0' < "$LOG" | grep -aE 'guest parked|EXEC_READY|session=|desktop ready|starting Plasma|Plasma failed|FLUSH|serial 2|invalid ELF|error while loading|fatal signal|Fatal server|exited with status|entering KDE|interpreter alive' | tail -n 50
echo
echo "===== Xorg tail markers ====="
tr -d '\r\0' < "$LOG" | grep -aE 'xorg:' | tail -n 25
