#!/usr/bin/env bash
LOG=/home/deshab/qemu_serial_loop1.log
T=$(mktemp)
tr -d '\r\0' < "$LOG" > "$T"
echo "===== traps / bus error / segfault lines ====="
grep -aE 'traps:|Bus error|bus error|segfault|SIGBUS' "$T" | tail -n 30
echo
echo "===== plasma.err dumped lines (all) ====="
grep -a 'deshab-plasma.err' "$T" | tail -n 40
echo
echo "===== dmesg: lines from session ====="
grep -a 'deshab-kde] dmesg:' "$T" | tail -n 15
echo
echo "===== FLUSH_N / scanout context ====="
grep -anE 'FLUSH_N|SET_SCANOUT|scanout' "$T" | tail -n 15
rm -f "$T"
