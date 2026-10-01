#!/usr/bin/env bash
set -euo pipefail
IMG="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
ESP=$(mktemp -d)
mount "${LOOP}p1" "$ESP"
cp -f "$ESP/boot/linux-rootfs.img" /tmp/linux-rootfs-check.img
umount "$ESP"
losetup -d "$LOOP"
rmdir "$ESP"
M=$(mktemp -d)
mount -o loop,ro /tmp/linux-rootfs-check.img "$M"
python3 - "$M" <<'PY'
import os, sys
root = sys.argv[1]
path = None
for cand in (root + "/usr/lib/libc.so.6", root + "/lib/libc.so.6"):
    if os.path.exists(cand):
        path = cand
        break
print("libc", path)
with open(path, "rb") as f:
    data = f.read()
off = 0x199140
print("size", len(data), "at", hex(off), data[off:off+16].hex())
print("endbr64 count", data.count(b"\xf3\x0f\x1e\xfa"))
print("badnop count", data.count(b"\x0f\x1f\x40\x00"))
print("nop90 at site", data[off:off+4] == b"\x90\x90\x90\x90")
PY
umount "$M"
rmdir "$M"
echo "===== df ====="
df -h /home /tmp | head
ls -lh "$IMG" /tmp/linux-rootfs-check.img
