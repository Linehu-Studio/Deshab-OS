#!/usr/bin/env bash
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify7_diag.txt
{
  echo "===== dmesg oom/kvm ====="
  dmesg -T 2>/dev/null | grep -iE 'oom|killed process|qemu|kvm' | tail -n 40 || true
  echo "===== mem ====="
  free -h
  echo "===== img ====="
  ls -lh /mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img
  echo "===== last limine ====="
} >"$OUT" 2>&1
python3 - <<'PY'
from pathlib import Path
b = Path("/home/deshab/qemu_serial_loop1.log").read_bytes()
ascii_only = bytes(c if 32 <= c < 127 or c in (10, 13, 9) else 32 for c in b)
t = ascii_only.decode("ascii")
# collapse limine cursor addressing to readable words
import re
plain = re.sub(r"\[[0-9;]*[A-Za-z]", "", t)
plain = re.sub(r" +", " ", plain)
Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify7_limine.txt").write_text(
    plain[-2500:], encoding="utf-8")
print("limine_plain", len(plain))
PY
echo DIAG_OK >>"$OUT"
