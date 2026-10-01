#!/usr/bin/env bash
# Symbolize 0xffffffff800669f0 (r10 in the GP fault dumps) + dump bytes there.
E=/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/utsm.elf
nm -n "$E" > /tmp/syms2.txt
python3 - <<'PY'
t = 0xffffffff800669f0
prev = None
with open("/tmp/syms2.txt") as f:
    for ln in f:
        p = ln.split()
        if len(p) >= 3:
            try:
                v = int(p[0], 16)
            except ValueError:
                continue
            if v > t:
                print("below:", prev)
                print("above:", ln.rstrip())
                break
            prev = ln.rstrip()
PY
echo "=== bytes at that address ==="
objdump -s --start-address=0xffffffff800669e0 --stop-address=0xffffffff80066a20 "$E" | tail -4