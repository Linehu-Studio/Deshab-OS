#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
echo '=== Plasma verdict ==='
T | grep -a -E 'Plasma (failed|session ended)|status 1[0-9][0-9]|fatal signal' | tail -4
echo '=== plasma-tail last file= entries (crash window) ==='
T | grep -a 'plasma-tail' | grep -a -E 'file=|entry:' | tail -12
echo '=== tail of plasma-tail ==='
T | grep -a 'plasma-tail' | tail -6
echo '=== dmesg segv/bus ==='
T | grep -a -E 'dmesg:|Bus error|segfault' | tail -5