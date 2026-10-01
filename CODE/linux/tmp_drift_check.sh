#!/usr/bin/env bash
# Newest UTSM sources — detect drift after the last good (Windows) build.
cd /mnt/d/Code/DEAICUP/Deshab/CODE/UTSM
find . \( -name '*.c' -o -name '*.S' -o -name '*.h' -o -name 'linker.ld' -o -name 'MAKEFILE' \) \
  -newermt '2026-09-14' -printf '%TY-%Tm-%Td %TH:%TM %p\n' | sort -r | head -20
echo '=== obj dir leftovers (old Windows .o timestamps) ==='
ls -la --time-style='+%m-%d %H:%M' arch/x86_64/*.o 2>/dev/null | head -5
echo '=== build config template FUCK (what build.ps1 would inject) ==='
grep -n 'demo_ms\|selftest' /mnt/d/Code/DEAICUP/Deshab/build/configs/dev/FUCK 2>/dev/null || echo 'no build/configs/dev/FUCK'
