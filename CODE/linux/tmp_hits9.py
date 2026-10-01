from pathlib import Path
from collections import Counter

p = Path("/home/deshab/qemu_serial_loop1.log")
b = p.read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
t = b.decode("utf-8", "replace")
lines = t.splitlines()
print("BYTES", len(b), "LINES", len(lines))

keys = (
    "esp file", "extra-rootfs", "persist", "startplasma", "entering KDE",
    "deshab-kde", "desktop ready", "WARNING", "ERROR", "bound", "vdc",
    "VBLK", "LINUX", "launch", "DSM", "ahci", "guest parked", "EXEC_READY",
    "switch_root", "utsm-linux", "idtr", "FAIL", "misconfig",
)
hits = [ln for ln in lines if any(k.lower() in ln.lower() for k in keys)]
print("HITS", len(hits))
print("===== first 80 hits =====")
print("\n".join(hits[:80]))
print("===== last 40 hits =====")
print("\n".join(hits[-40:]))

ctr = Counter(ln.split("=")[0][:60] if "=" in ln else ln[:60] for ln in lines)
print("===== top prefixes =====")
for k, n in ctr.most_common(20):
    print(n, k)
