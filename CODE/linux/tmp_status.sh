#!/usr/bin/env bash
set +e
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_dump7.txt
LOG=/home/deshab/qemu_serial_loop1.log
{
    echo "===== size ====="
    ls -l "$LOG"
    echo "===== desktop / session ====="
    tr -d '\r\0' < "$LOG" | grep -E 'desktop |session=|serial 2|KDE |EXEC_READY|parked|FUCK config|jumping to DSK|user/desktop' | head -n 80
    echo "===== last 40 ====="
    tr -d '\r\0' < "$LOG" | tail -n 40
} > "$OUT" 2>&1
echo WROTE
exit 0
