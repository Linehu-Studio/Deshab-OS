from pathlib import Path
p = Path("/home/deshab/qemu_serial_loop1.log")
b = p.read_bytes().replace(b"\x00", b"").replace(b"\r", b"")
t = b.decode("utf-8", "replace")
want = ("[LINUX]", "[VBLK]", "extra-rootfs", "launch", "parked", "EXEC_READY",
        "Linux loader", "Linux launch", "misconfig", "VMLAUNCH", "vmlaunch",
        "[VIO]", "slot6", "ESP", "BLOCK]", "bound")
for ln in t.splitlines():
    if any(k in ln for k in want):
        print(ln)
