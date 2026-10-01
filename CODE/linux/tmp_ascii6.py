#!/usr/bin/env python3
from pathlib import Path
b = Path("/home/deshab/qemu_serial_loop1.log").read_bytes()
ascii_only = bytes(c if 32 <= c < 127 or c in (10, 13, 9) else 32 for c in b)
t = ascii_only.decode("ascii")
Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify6_ascii.txt").write_text(
    t[:5000] + "\n---TAIL---\n" + t[-3000:], encoding="utf-8"
)
print("ok", len(b), "ascii_nonspace", sum(1 for c in ascii_only if c not in (32, 10, 13, 9)))
