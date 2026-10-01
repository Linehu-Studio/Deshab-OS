#!/usr/bin/env bash
# Restore persist ld.so/libc from the native extra copy (objcopy left
# zero-size GNU_PROPERTY headers). Then re-inject session + helper.
set -uo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
OUT="$ROOT/.build_tmp/restore_persist_ld.out"
exec > >(stdbuf -oL tee "$OUT") 2>&1
killall qemu-system-x86_64 2>/dev/null || true

WANT_LD=1e794eb14f4bccf6186bf8d9aa2018e8963ea668
WANT_LIBC=503200d7fda94a5dc6058d7e0694e5d1dcb2e372
SRC=""
S=""
for p in /home/deshab/linux-extra-rootfs.img /root/extra_rootfs_work/extra-rootfs.img; do
    [[ -f "$p" ]] || continue
    echo "[restore] candidate $p"
    t=$(mktemp -d)
    if ! mount -o loop,ro "$p" "$t"; then
        rmdir "$t"
        continue
    fi
    bid=$(readelf -n "$t/usr/lib/ld-linux-x86-64.so.2" 2>/dev/null | awk '/Build ID:/ {print $3; exit}')
    echo "[restore] $p ld.so buildid=$bid"
    readelf -l "$t/usr/lib/ld-linux-x86-64.so.2" 2>/dev/null | grep -A1 GNU_PROPERTY || true
    if [[ "$bid" == "$WANT_LD" ]]; then
        SRC="$p"
        S="$t"
        break
    fi
    umount "$t"
    rmdir "$t"
done
if [[ -z "$SRC" ]]; then
    echo "[restore] no matching native extra copy (want $WANT_LD)"
    exit 1
fi
echo "[restore] src $SRC"

IMG=/home/deshab/deshab-dev.img
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount "${LOOP}p1" "$M"
E=$(mktemp -d)
mount -o loop "$M/boot/linux-extra-rootfs.img" "$E"
echo "[restore] dst before"
readelf -l "$E/usr/lib/ld-linux-x86-64.so.2" 2>&1 | grep -A1 GNU_PROPERTY || true
cp -a "$S/usr/lib/ld-linux-x86-64.so.2" "$E/usr/lib/ld-linux-x86-64.so.2"
# libc.so.6 may be a symlink to libc.so.6 real file
src_libc=$(readlink -f "$S/usr/lib/libc.so.6")
dst_libc=$(readlink -f "$E/usr/lib/libc.so.6")
echo "[restore] libc $src_libc -> $dst_libc"
cp -a "$src_libc" "$dst_libc"
chmod 0755 "$E/usr/lib/ld-linux-x86-64.so.2" "$dst_libc"
echo "[restore] dst after"
readelf -l "$E/usr/lib/ld-linux-x86-64.so.2" 2>&1 | grep -A2 GNU_PROPERTY || true
readelf -n "$E/usr/lib/ld-linux-x86-64.so.2" 2>&1 | grep -i 'x86 feature\|SHSTK\|IBT\|GNU_PROPERTY' || true
umount "$E"
rmdir "$E"
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
umount "$S"
rmdir "$S"
echo "[restore] extra ld.so restored"
