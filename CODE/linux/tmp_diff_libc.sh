#!/usr/bin/env bash
set -u
IMG=/home/deshab/deshab-dev.img
L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M"
E=$(mktemp -d); R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-extra-rootfs.img" "$E"
mount -o loop,ro "$M/boot/linux-rootfs.img" "$R"
cp "$R/usr/lib/libc.so.6" /tmp/libc.vdb
cp "$E/usr/lib/libc.so.6" /tmp/libc.persist
umount "$R"; umount "$E"; umount "$M"
rmdir "$R" "$E" "$M"
losetup -d "$L"
python3 - <<'PY'
a = open('/tmp/libc.vdb','rb').read()
b = open('/tmp/libc.persist','rb').read()
print("len", len(a), len(b))
diffs = [i for i in range(min(len(a),len(b))) if a[i] != b[i]]
print("num_diff_bytes", len(diffs))
if diffs:
    # group into ranges
    ranges = []
    s = p = diffs[0]
    for i in diffs[1:]:
        if i == p + 1:
            p = i
        else:
            ranges.append((s, p)); s = p = i
    ranges.append((s, p))
    print("num_ranges", len(ranges))
    for s, e in ranges[:40]:
        print(f"  range 0x{s:x}-0x{e:x} ({e-s+1} bytes) page=0x{s & ~0xfff:x}")
PY
