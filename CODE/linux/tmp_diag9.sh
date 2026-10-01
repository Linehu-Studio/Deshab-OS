#!/usr/bin/env bash
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify9_diag.txt
{
  echo "===== dmesg ====="
  dmesg -T | tail -n 50
  echo "===== free ====="
  free -h
  echo "===== qemu ====="
  pgrep -a qemu || echo none
  echo "===== verify9 out bytes ====="
  wc -c /mnt/d/Code/DEAICUP/Deshab/.build_tmp/wsl_verify9.out
} >"$OUT" 2>&1
echo DIAG_OK
