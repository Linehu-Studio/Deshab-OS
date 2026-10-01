#!/usr/bin/env bash
set -uo pipefail
PKG=/mnt/d/Code/DEAICUP/Deshab/.build_tmp/pkgs
DEST=/home/deshab/xorg-libs
MOD=/home/deshab/xorg-input
mkdir -p "$DEST" "$MOD"
stage=/tmp/input-stage
rm -rf "$stage"
mkdir -p "$stage"
for f in \
  libinput-1.31.3-1-x86_64.pkg.tar.zst \
  libevdev-1.13.7-1-x86_64.pkg.tar.zst \
  mtdev-1.1.7-1-x86_64.pkg.tar.zst \
  xf86-input-libinput-1.5.0-1-x86_64.pkg.tar.zst
 do
  tar -C "$stage" -xf "$PKG/$f"
 done
cp -a "$stage"/usr/lib/libinput.so* "$stage"/usr/lib/libevdev.so* "$stage"/usr/lib/libmtdev.so* "$DEST"/
install -D -m 0755 "$stage"/usr/lib/xorg/modules/input/libinput_drv.so \
  "$MOD/libinput_drv.so"
echo "[input] libs=$(ls "$DEST"/libinput.so* "$DEST"/libevdev.so* "$DEST"/libmtdev.so* | wc -l)"
file "$DEST"/libinput.so.10 "$DEST"/libevdev.so.2 "$DEST"/libmtdev.so.1 "$MOD/libinput_drv.so"
echo "===== libinput NEEDED ====="
readelf -d "$DEST"/libinput.so.10 | grep NEEDED || true
echo "[input] xorg-libs=$(ls "$DEST" | wc -l)"
# Do not set MODESET: that path is modesetting_drv.so, not the input driver.
export XORG_LIBS="$DEST"
bash /mnt/d/Code/DEAICUP/Deshab/CODE/linux/tmp_patch_session.sh
