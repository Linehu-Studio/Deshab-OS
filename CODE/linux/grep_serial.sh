#!/usr/bin/env bash
set -euo pipefail
LOG=/home/deshab/qemu_serial_loop1.log
clean() { tr -d '\r\0' < "$LOG"; }
echo "===== skip ENDBR / exception ====="
clean | grep -E 'skip ENDBR|exception rip=|exception intr_info=|insn0=' | head -n 80 || true
echo "===== first user-looking rip ====="
clean | grep 'exception rip=' | grep -v 'ffffffff' | head -n 20 || true
echo "===== GP count ====="
clean | grep -c 'general protection fault' || true
echo "===== deshab-kde / ping / exec / serial ====="
clean | grep -E 'skip landing|CET emulate|deshab-kde|guest ping|exec_async|exec rc|SKIP:|KDE spawned|serial 2|launch KDE|busybox|segfault|interpreter alive|CET U_CET|user-exc|MOV CR3|EPT reuse' | head -n 80 || true
echo "===== skip landing / user exception ====="
clean | grep -E 'skip landing|skip ENDBR|exception rip=0x00007' | head -n 40 || true

