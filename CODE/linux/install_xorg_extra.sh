#!/usr/bin/env bash
# Install Xorg into the Arch extra-rootfs so startplasma-x11 has a server.
set -Eeuo pipefail
IMG="${1:-/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/linux-extra-rootfs.img}"
M=$(mktemp -d)
mount -o loop "$IMG" "$M"
cleanup() {
    set +e
    umount "$M/dev" 2>/dev/null || true
    umount "$M/proc" 2>/dev/null || true
    umount "$M/sys" 2>/dev/null || true
    umount "$M" 2>/dev/null || true
    rmdir "$M" 2>/dev/null || true
}
trap cleanup EXIT
mount --bind /dev "$M/dev"
mount -t proc proc "$M/proc"
mount -t sysfs sys "$M/sys"
if [[ -f /etc/resolv.conf ]]; then
    mkdir -p "$M/etc"
    cp -L /etc/resolv.conf "$M/etc/resolv.conf" || true
fi
if [[ -x "$M/usr/bin/Xorg" ]]; then
    echo "[xorg] already present"
    exit 0
fi
echo "[xorg] pacman -S xorg-server xorg-xinit"
chroot "$M" /usr/bin/pacman -Sy --noconfirm xorg-server xorg-xinit
test -x "$M/usr/bin/Xorg"
echo "[xorg] installed $IMG"
