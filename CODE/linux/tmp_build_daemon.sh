#!/usr/bin/env bash
set -euo pipefail
REPO=/mnt/d/Code/DEAICUP/Deshab
SRC="$REPO/CODE/linux/initramfs/bin/utsm_exec_daemon.c"
OUT="$REPO/.build_tmp/utsm_exec_daemon"
mkdir -p "$REPO/.build_tmp"
sed -i 's/\r$//' "$SRC"
cc -static -O2 -Wall -Wextra -I"$REPO/CODE/utsm-ipc" -o "$OUT" "$SRC"
strip "$OUT" 2>/dev/null || true
ls -l "$OUT"
echo DAEMON_OK
