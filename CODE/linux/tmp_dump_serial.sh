#!/usr/bin/env bash
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
LOG=/home/deshab/qemu_serial_loop1.log
OUT="$ROOT/.build_tmp/verify_timeslice.txt"
python3 - <<'PY'
from pathlib import Path
src = Path("/home/deshab/qemu_serial_loop1.log")
dst = Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice.txt")
data = src.read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
text = data.decode("utf-8", "replace")
keys = (
    "EXEC_READY", "guest parked", "session=", "desktop ready", "serial 2",
    "deshab-kde", "vdc", "persist", "KDE ", "scanout", "starting Plasma",
    "entering KDE", "async spawn", "execv failed", "VERIFY_", "checking persist",
    "interpreter alive", "[DSK]", "desktop load", "FirstInit", "firstInit",
    "dsk", "DSK", "FUCK", "switch_root", "HLT", "timeslice", "no EXEC_EXIT",
    "linux_resume failed", "segfault", "PANIC", "oops", "desktop.elf",
    "chooser", "waiting for 1",
)
lines = []
for line in text.splitlines():
    if any(k.lower() in line.lower() for k in keys):
        lines.append(line)
dst.write_text("\n".join(lines[-200:]) + f"\n\nLOG_BYTES={src.stat().st_size}\nMATCHED={len(lines)}\n", encoding="utf-8")
print(f"wrote {dst} matches={len(lines)} bytes={src.stat().st_size}")
PY
