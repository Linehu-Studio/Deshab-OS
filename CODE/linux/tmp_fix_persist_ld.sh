#!/usr/bin/env bash
# Restore persist ld.so/libc if objcopy broke them; report extra copies.
set -uo pipefail
OUT=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/fix_persist_ld.out
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true

echo "===== extra-rootfs candidates ====="
for p in \
    /root/extra_rootfs_work/extra-rootfs.img \
    /home/deshab/linux-extra-rootfs.img \
    /home/deshab/extra-check.img \
    /root/extra_rootfs_work/linux-extra-rootfs.img; do
    if [[ -f "$p" ]]; then
        echo "EXISTS $p $(stat -c %s "$p")"
        debugfs -R 'stat /usr/lib/ld-linux-x86-64.so.2' "$p" 2>/dev/null | head -n 8 || true
    else
        echo "MISSING $p"
    fi
done

echo "===== native GPT extra ld.so ====="
IMG=/home/deshab/deshab-dev.img
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount "${LOOP}p1" "$M"
EXTRA="$M/boot/linux-extra-rootfs.img"
ls -lh "$EXTRA"
E=$(mktemp -d)
if mount -o loop "$EXTRA" "$E"; then
    echo "[extra] file ld.so"
    file "$E/usr/lib/ld-linux-x86-64.so.2" "$E/usr/lib/libc.so.6" || true
    echo "[extra] readelf ld.so notes"
    readelf -n "$E/usr/lib/ld-linux-x86-64.so.2" 2>&1 | head -n 40 || true
    echo "[extra] readelf ld.so program headers"
    readelf -l "$E/usr/lib/ld-linux-x86-64.so.2" 2>&1 | head -n 30 || true
    echo "[extra] ls env bash"
    ls -l "$E/usr/bin/env" "$E/usr/bin/bash" "$E/bin/bash" "$E/usr/local/bin/deshab-kde-session" || true
    umount "$E"
fi
rmdir "$E"
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo DONE
