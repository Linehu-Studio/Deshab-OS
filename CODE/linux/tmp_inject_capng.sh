set -uo pipefail
tar -C /tmp -xf /mnt/d/Code/DEAICUP/Deshab/.build_tmp/pkgs/libcap-ng.pkg.tar.zst usr/lib/libcap-ng.so usr/lib/libcap-ng.so.0 usr/lib/libcap-ng.so.0.0.0
cp -a /tmp/usr/lib/libcap-ng.so /tmp/usr/lib/libcap-ng.so.0 /tmp/usr/lib/libcap-ng.so.0.0.0 /home/deshab/xorg-libs/
echo "[capng] $(ls -l /home/deshab/xorg-libs/libcap-ng.so* | tr '\n' ' ')"
file /home/deshab/xorg-libs/libcap-ng.so.0.0.0
export XORG_LIBS=/home/deshab/xorg-libs
bash /mnt/d/Code/DEAICUP/Deshab/CODE/linux/tmp_patch_session.sh