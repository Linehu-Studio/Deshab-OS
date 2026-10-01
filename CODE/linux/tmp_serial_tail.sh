#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
echo '=== selftest ==='
T | grep -a 'SELFTEST\|block/wake'
echo '=== last 14 lines ==='
T | tail -14
echo '=== exception/panic markers ==='
T | grep -a -n 'IDT. exception\|vector=0x000000000000000d\|PANIC\|panic\|dsk_load\|DSK jump' | tail -6