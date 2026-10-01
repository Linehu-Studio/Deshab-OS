#!/usr/bin/env bash
python3 - <<'PY'
from pathlib import Path
p = Path("/home/deshab/qemu_serial_loop1.log")
b = p.read_bytes()
dst = Path("/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_timeslice_all.txt")
head = b[:200]
# printable
def vis(x):
    return "".join(chr(c) if 32 <= c < 127 else f"[{c:02x}]" for c in x)
text = b.replace(b"\x00", b"").replace(b"\r", b"").decode("latin1", "replace")
lines = [ln for ln in text.splitlines() if ln.strip()]
dst.write_text(
    "HEAD="+vis(head)+"\n\nNON_EMPTY="+str(len(lines))+"\n"+
    "\n".join(lines[:80])+"\n",
    encoding="utf-8",
)
print("nonzero", sum(1 for c in b if c), "empty_lines", text.count("\n"), "nonempty", len(lines))
PY
