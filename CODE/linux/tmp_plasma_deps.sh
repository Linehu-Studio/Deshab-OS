#!/usr/bin/env bash
set -uo pipefail
IMG=/home/deshab/deshab-dev.img
killall qemu-system-x86_64 2>/dev/null || true
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${LOOP}p1" "$M"
EXTRA="$M/boot/linux-extra-rootfs.img"
W=/tmp/plasma-deps
mkdir -p "$W"
debugfs -R "dump /usr/lib/libpam.so.0.85.1 $W/pam" "$EXTRA" >/dev/null
debugfs -R "dump /usr/lib/libpam_misc.so.0.82.1 $W/pammisc" "$EXTRA" >/dev/null
echo "===== pam NEEDED ====="
readelf -d "$W/pam" "$W/pammisc" | grep NEEDED || true
magic_of() {
  local path="$1"
  local dest sz mag
  dest=$(debugfs -R "stat $path" "$EXTRA" 2>/dev/null | sed -n 's/.*Fast link dest: "//p' | tr -d '"')
  if [[ -n "$dest" ]]; then
    [[ "$dest" != /* ]] && dest="$(dirname "$path")/$dest"
    path="$dest"
  fi
  sz=$(debugfs -R "stat $path" "$EXTRA" 2>/dev/null | sed -n 's/.*Size: *//p' | awk '{print $1; exit}')
  mag=$(debugfs -R "cat $path" "$EXTRA" 2>/dev/null | head -c 4 | od -An -tx1 | tr -s ' ')
  echo "$path size=${sz:-?} magic=$mag"
}
while read -r name; do
  [[ -n "$name" ]] || continue
  magic_of "/usr/lib/$name"
done < <(readelf -d "$W/pam" "$W/pammisc" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort -u)
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo DONE
