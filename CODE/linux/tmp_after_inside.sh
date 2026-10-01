#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
N=$(T | grep -a -n 'inside-bash' | tail -1 | cut -d: -f1)
echo "inside-bash at serial line $N"
T | sed -n "${N},$((N + 40))p" | grep -a -v 'FLUSH_N\|flush count' | head -34