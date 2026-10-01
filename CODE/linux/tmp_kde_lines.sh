#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
N=$(T | grep -a -n 'inside-bash' | tail -1 | cut -d: -f1)
T | tail -n +$N | grep -a 'deshab-kde' | grep -a -v 'plasma-tail' | tail -22