#!/usr/bin/env bash
set -u
IMG=/home/deshab/deshab-dev.img
L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M"
ls -la "$M/boot/"
E=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-extra-rootfs.img" "$E"
echo "== persist ld.so / libc / dbus-run-session =="
ls -la "$E"/lib64/ld-linux-x86-64.so.2 "$E"/usr/lib/ld-linux-x86-64.so.2 "$E"/usr/lib/libc.so.6 "$E"/usr/bin/dbus-run-session "$E"/usr/lib/libdbus-1.so.3.38.3 2>&1
echo
for f in "$E"/usr/lib/ld-linux-x86-64.so.2 "$E"/usr/lib/libc.so.6 "$E"/usr/bin/dbus-run-session "$E"/usr/lib/libdbus-1.so.3.38.3; do
    echo "-- $f"
    file "$f"
    readelf -h "$f" 2>/dev/null | grep -E 'Entry|Type|Machine' || true
done
echo
echo "== md5 comparison vdb vs persist libc =="
R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-rootfs.img" "$R"
md5sum "$R"/usr/lib/libc.so.6 "$E"/usr/lib/libc.so.6 2>&1
md5sum "$R"/usr/lib/libdbus-1.so.3.38.3 "$E"/usr/lib/libdbus-1.so.3.38.3 2>&1
umount "$R"; rmdir "$R"
umount "$E"; rmdir "$E"
umount "$M"; rmdir "$M"
losetup -d "$L"
echo DONE
