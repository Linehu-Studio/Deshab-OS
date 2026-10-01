#!/usr/bin/env bash
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
killall qemu-system-x86_64 2>/dev/null || true
sleep 3
ss -ltnp | grep -E '45454|45455' || true
export QEMU_SERIAL_LOG=/home/deshab/qemu_serial_loop1.log
export QEMU_VERIFY_TIMEOUT=480
sed -i 's/\r$//' "$ROOT/CODE/linux/qemu_verify_loop.sh"
bash "$ROOT/CODE/linux/qemu_verify_loop.sh"
rc=$?
python3 "$ROOT/CODE/linux/tmp_ascii.py" || true
{
  echo "verify_exit=$rc"
  python3 - <<'PY'
from pathlib import Path
b = Path("/home/deshab/qemu_serial_loop1.log").read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
t = b.decode("utf-8", "replace")
keys = ("EXEC_READY", "guest parked", "session=", "desktop ready", "deshab-kde",
        "entering KDE", "starting Plasma", "vscode", "IDT", "LNXC", "VERIFY_")
lines = [ln for ln in t.splitlines() if any(k.lower() in ln.lower() for k in keys)]
Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice.txt").write_text(
    "\n".join(lines[-180:]) + f"\nLOG_BYTES={len(b)}\nMATCHED={len(lines)}\n", encoding="utf-8")
print("matches", len(lines), "bytes", len(b))
PY
} | tee /mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_rc.txt
exit $rc
