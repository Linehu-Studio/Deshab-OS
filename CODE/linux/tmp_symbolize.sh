#!/usr/bin/env bash
# Correctly symbolize GP fault RIP 0xffffffff800353e1 and identify the stub.
set -uo pipefail
E=/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/utsm.elf
nm -n "$E" > /tmp/syms.txt
python3 - <<'PY'
t = 0xffffffff800353e1
lines = []
with open("/tmp/syms.txt") as f:
    for ln in f:
        p = ln.split()
        if len(p) >= 3:
            try:
                v = int(p[0], 16)
            except ValueError:
                continue
            lines.append((v, ln.rstrip()))
prev = None
for v, s in lines:
    if v > t:
        print("FAULT inside/below:", prev)
        print("next symbol:", s)
        break
    prev = s
PY
echo "=== stub entry (search backwards for vector push) ==="
objdump -d --start-address=0xffffffff80035280 --stop-address=0xffffffff800353d8 "$E" | grep -E 'push|jmp|call|mov.*0x' | head -20