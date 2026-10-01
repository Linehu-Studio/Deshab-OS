#!/usr/bin/env bash
# Compare loop-mount view vs debugfs view of one "BAD" lib in extra-rootfs.
set -uo pipefail
IMG="${1:?usage: tmp_dbg_one.sh IMG}"
NAME="${2:-libz.so.1.3.2}"
OUT=/tmp/deshab-dbgone
rm -rf "$OUT"; mkdir -p "$OUT"

L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M" || { echo ESP_MOUNT_FAIL; exit 1; }
# Copy the extra-rootfs image OFF drvfs first (loop off drvfs is suspect).
cp "$M/boot/linux-extra-rootfs.img" "$OUT/extra.img"
umount "$M"; rmdir "$M"; losetup -d "$L"

echo "=== debugfs view (no kernel fs) ==="
debugfs -R "dump /usr/lib/$NAME" "$OUT/df.$NAME" "$OUT/extra.img" 2>&1 | tail -2
ls -l "$OUT/df.$NAME" 2>/dev/null
head -c 64 "$OUT/df.$NAME" 2>/dev/null | od -A d -t x1 | head -5

echo "=== loop-mount view (kernel ext4) ==="
R=$(mktemp -d)
if mount -o loop,ro "$OUT/extra.img" "$R"; then
    ls -l "$R/usr/lib/$NAME"
    head -c 64 "$R/usr/lib/$NAME" | od -A d -t x1 | head -5
    md5sum "$R/usr/lib/$NAME" "$OUT/df.$NAME"
    umount "$R"
else
    echo "loop mount FAILED"
fi
rmdir "$R"
readelf -h "$OUT/df.$NAME" 2>&1 | grep -E 'Class|Type|Entry|program headers|section header size' || echo "readelf failed on debugfs copy"