#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
echo '=== Plasma phase ==='
T | grep -a -E 'starting Plasma|Plasma (failed|session ended)|status 1[0-9][0-9]|FLUSH_N|flush count|kwin|plasmashell' | tail -12
echo '=== FLUSH context (lines around) ==='
T | grep -a -B3 -A3 'flush count' | tail -14
echo '=== late-session markers ==='
T | grep -a -E 'Xorg socket|Xorg pid|seatd_sock|entering KDE|deshab-kde. (chroot|interpreter)' | tail -6