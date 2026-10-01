#!/usr/bin/env bash
T() { tr -d '\r\0' < /home/deshab/qemu_serial_loop1.log; }
echo '=== Xorg phase ==='
T | grep -a 'deshab-kde' | grep -a -E 'Xorg|ldpath' | tail -8
echo '=== fatal signal dumps (Xorg) ==='
T | grep -a -A14 'Xorg.*fatal signal\|Xorg: .*segfault\|Xorg: .*general protection' | head -30
echo '=== any segfault/GP lines ==='
T | grep -a -E 'segfault|general protection|fatal signal' | tail -6
echo '=== xorg.err dump ==='
T | grep -a 'xorg.err' | tail -3