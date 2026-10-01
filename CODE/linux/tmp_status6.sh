#!/usr/bin/env bash
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_status6.txt
{
  echo "===== date ====="
  date
  echo "===== qemu ====="
  pgrep -a qemu-system-x86_64 || echo "no qemu"
  echo "===== ports ====="
  ss -ltnp | grep -E '45454|45455' || echo "no serial/mon ports"
  echo "===== serial log ====="
  ls -l /home/deshab/qemu_serial_loop1.log 2>/dev/null || echo "no log"
} >"$OUT" 2>&1
python3 - <<'PY'
from pathlib import Path
out = Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_kde_markers6.txt")
p = Path("/home/deshab/qemu_serial_loop1.log")
if not p.exists():
    out.write_text("NO_SERIAL_LOG\n", encoding="utf-8")
    raise SystemExit
b = p.read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
t = b.decode("utf-8", "replace")
keys = ("EXEC_READY", "guest parked", "session=", "desktop ready", "deshab-kde",
        "entering KDE", "starting Plasma", "persist", "vdc", "VERIFY_",
        "serial 2", "LNXC", "EXEC_EXIT", "switch_root", "WARNING")
lines = [ln for ln in t.splitlines() if any(k.lower() in ln.lower() for k in keys)]
out.write_text("\n".join(lines[-220:]) + f"\nLOG_BYTES={len(b)}\nMATCHED={len(lines)}\n", encoding="utf-8")
print("wrote", out, "bytes", len(b), "matches", len(lines))
PY
echo STATUS_OK
