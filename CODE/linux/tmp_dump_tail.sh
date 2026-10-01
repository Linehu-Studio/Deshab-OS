#!/usr/bin/env bash
set -euo pipefail
python3 - <<'PY'
from pathlib import Path
src = Path("/home/deshab/qemu_serial_loop1.log")
data = src.read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
text = data.decode("utf-8", "replace")
lines = text.splitlines()
dst = Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice_tail.txt")
tail = lines[-80:]
dst.write_text("\n".join(tail) + f"\n\nTOTAL_LINES={len(lines)}\n", encoding="utf-8")
print("lines", len(lines), "tail", len(tail))
# also search DSK / desktop / load
hits = [ln for ln in lines if "DSK" in ln or "desktop" in ln or "dsk" in ln or "LNXC" in ln or "jumping" in ln]
Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice_dsk.txt").write_text("\n".join(hits[-80:]) + f"\nHITS={len(hits)}\n", encoding="utf-8")
print("dsk hits", len(hits))
PY
