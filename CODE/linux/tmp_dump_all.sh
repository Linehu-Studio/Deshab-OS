#!/usr/bin/env bash
python3 - <<'PY'
from pathlib import Path
p = Path("/home/deshab/qemu_serial_loop1.log")
b = p.read_bytes()
print("bytes", len(b))
text = b.replace(b"\x00", b"").replace(b"\r", b"").decode("utf-8", "replace")
lines = text.splitlines()
print("lines", len(lines))
out = Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice_all.txt")
out.write_text("\n".join(lines[:40] + ["..."] + lines[-60:]) + f"\n\nBYTES={len(b)} LINES={len(lines)}\n", encoding="utf-8")
PY
