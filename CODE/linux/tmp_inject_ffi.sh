set -uo pipefail
tar -C /tmp -xf /mnt/d/Code/DEAICUP/Deshab/.build_tmp/pkgs/libffi-3.8.0-1-x86_64.pkg.tar.zst
cp -a /tmp/usr/lib/libffi.so* /home/deshab/xorg-libs/
ls -l /home/deshab/xorg-libs/libffi.so* 
export XORG_LIBS=/home/deshab/xorg-libs
bash /mnt/d/Code/DEAICUP/Deshab/CODE/linux/tmp_patch_session.sh