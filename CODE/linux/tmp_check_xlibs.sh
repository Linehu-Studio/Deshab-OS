#!/usr/bin/env bash
D=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/xorg-libs
echo "== exists? =="
ls -ld "$D" 2>&1
echo "== count =="
ls "$D" 2>/dev/null | wc -l
echo "== libc / libdbus / ld-linux in overlay =="
ls "$D" 2>/dev/null | grep -E 'libc|libdbus|ld-linux' || echo "(none)"
echo "== full listing head =="
ls "$D" 2>/dev/null | head -50
echo "== .build_tmp top =="
ls -la /mnt/d/Code/DEAICUP/Deshab/.build_tmp/ | head -40
