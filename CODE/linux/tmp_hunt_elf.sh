#!/usr/bin/env bash
# Hunt for surviving copies of the last-good utsm.elf (md5 3c5d359a...).
md5sum /mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/utsm.elf
find /mnt/d/Code/DEAICUP/Deshab /home/deshab /tmp -maxdepth 4 -name '*utsm*.elf*' -o -maxdepth 4 -name 'utsm.elf.*' 2>/dev/null | while read -r f; do
    [ -f "$f" ] && echo "$(md5sum "$f" | cut -d' ' -f1) $(stat -c '%s %y' "$f" | cut -d'.' -f1) $f"
done | sort -u | head -20
echo '=== any backup dirs ==='
ls /mnt/d/Code/DEAICUP/Deshab/.build_tmp/ | grep -iE 'utsm|backup|bak' | head -10
ls /home/deshab/ | grep -iE 'utsm|bak|old' | head -10