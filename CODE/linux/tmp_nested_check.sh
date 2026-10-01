#!/usr/bin/env bash
# Check the *on-disk* health of the nested Linux payloads inside the ESP:
#   linux-rootfs.img, linux-extra-rootfs.img
# If a .so is truncated here, the guest SIGBUS is a build-time artifact.
# If everything is clean here, the corruption happens on the guest block path.
# Usage: bash tmp_nested_check.sh [esp-mount]
set -u
ESP=${1:-/mnt/deshab_p1}
RF=$ESP/boot/linux-rootfs.img
XRF=$ESP/boot/linux-extra-rootfs.img
M1=/mnt/deshab_rf
M2=/mnt/deshab_xrf

umount -q "$M1" 2>/dev/null
umount -q "$M2" 2>/dev/null
mkdir -p "$M1" "$M2"

echo "=== fsck rootfs (read-only, no repair)"
e2fsck -fn "$RF" 2>&1 | tail -15
echo "=== fsck extra-rootfs (read-only, no repair)"
e2fsck -fn "$XRF" 2>&1 | tail -15

L1=$(losetup --find --show "$RF")
L2=$(losetup --find --show "$XRF")
echo "=== loops $L1 $L2"
mount -o ro "$L1" "$M1" || { echo "mount rootfs failed"; exit 1; }
mount -o ro "$L2" "$M2" || { echo "mount extra-rootfs failed"; exit 1; }

echo "=== rootfs top"
ls -la "$M1"
echo "=== extra-rootfs top"
ls -la "$M2"
echo "=== extra-rootfs dirs of interest"
ls -la "$M2/usr" "$M2/usr/bin" 2>/dev/null | head -50
echo "=== extra-rootfs local libs"
ls -la "$M2/usr/local/lib" 2>/dev/null | head -20
ls -la "$M2/usr/local/lib/xorg-libs" 2>/dev/null | head -20

report() {
  local f=$1
  if [ ! -e "$f" ]; then echo "MISSING $f"; return; fi
  local kind size
  size=$(stat -c %s "$f")
  kind=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
  local dync
  dync=$(readelf -d "$f" 2>/dev/null | grep -c NEEDED)
  local err
  err=$(readelf -h "$f" 2>&1 >/dev/null | head -1)
  printf '%-70s size=%-10s magic=%-8s NEEDED=%-3s %s\n' "$f" "$size" "$kind" "$dync" "$err"
}
export -f report

echo "=== key X11/KDE ELFs on extra-rootfs"
for f in \
  "$M2/usr/bin/startplasma-x11" "$M2/usr/bin/plasmashell" "$M2/usr/bin/ksmserver" \
  "$M2/usr/bin/kwin_x11" "$M2/usr/bin/Xorg" "$M2/usr/lib/Xorg" \
  "$M2/usr/lib/libdbus-1.so.3" "$M2/usr/lib/libKF5Plasma.so.5" ; do
  report "$f"
done
echo "=== key X11/KDE ELFs on rootfs"
for f in "$M1/usr/bin/Xorg" "$M1/usr/lib/Xorg" "$M1/usr/bin/startplasma-x11"; do
  report "$f"
done

echo "=== truncation sweep: ELFs whose size is 0 or unreadable headers"
find "$M1" "$M2" -xdev -type f \( -name '*.so*' -o -perm -u+x \) -size -64k 2>/dev/null | head -40
echo "=== zero-size files"
find "$M1" "$M2" -xdev -type f -size 0 2>/dev/null | head -40
echo "=== df"
df -h "$M1" "$M2"