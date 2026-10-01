#!/bin/bash
#
# Install Arch distro CPython + hello.py into an existing guest root tree.
# Prefer a native-disk staging directory (not /mnt/d).
#
# Usage (as root):
#   bash CODE/linux/install_guest_python.sh
#   ROOTFS=/root/arch_rootfs_work/rootfs bash CODE/linux/install_guest_python.sh
#   ROOTFS=/root/extra_rootfs_work/mnt bash CODE/linux/install_guest_python.sh

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
ROOTFS="${ROOTFS:-/root/arch_rootfs_work/rootfs}"
HELLO_SRC="$PROJECT_ROOT/SYSTEM/user/python/hello.py"

if [ "$(id -u)" -ne 0 ]; then
    echo "[guest-python] ERROR: must run as root" >&2
    exit 1
fi
if [ ! -x "$ROOTFS/usr/bin/pacman" ] && [ ! -x "$ROOTFS/bin/pacman" ]; then
    echo "[guest-python] ERROR: $ROOTFS is not an Arch root (no pacman)" >&2
    exit 1
fi

# uutils mkdir (Ubuntu WSL) can error "Already exists" on -p; ignore that.
ensure_dir() {
    [ -d "$1" ] && return 0
    /usr/bin/mkdir -p "$1" 2>/dev/null || true
    [ -d "$1" ]
}
ensure_dir "$ROOTFS/dev"
ensure_dir "$ROOTFS/proc"
ensure_dir "$ROOTFS/sys"
ensure_dir "$ROOTFS/tmp"
# Deshab staging rootfs often symlinks /var/lib/pacman -> /tmp/pacman/lib.
ensure_dir "$ROOTFS/tmp/pacman/lib/sync"
ensure_dir "$ROOTFS/tmp/pacman/cache/pkg"
# Arch dropped [community]; leftover configs 404 on every -Sy.
if [ -f "$ROOTFS/etc/pacman.conf" ]; then
    /usr/bin/sed -i '/^\[community\]/,/^$/d' "$ROOTFS/etc/pacman.conf" || true
fi
printf 'nameserver 1.1.1.1\n' > "$ROOTFS/tmp/resolv.conf"
printf 'nameserver 1.1.1.1\n' > "$ROOTFS/etc/resolv.conf"
/bin/mount --bind /dev "$ROOTFS/dev" 2>/dev/null || true
/bin/mount --bind /proc "$ROOTFS/proc" 2>/dev/null || true
/bin/mount --bind /sys "$ROOTFS/sys" 2>/dev/null || true
cleanup() {
    set +e
    umount "$ROOTFS/dev" 2>/dev/null || true
    umount "$ROOTFS/proc" 2>/dev/null || true
    umount "$ROOTFS/sys" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

chroot "$ROOTFS" /usr/bin/pacman -Sy --noconfirm
# Staging trees often already contain overlapping bootstrap files.
chroot "$ROOTFS" /usr/bin/pacman -S --noconfirm --needed --overwrite '*' python

/bin/mkdir -p "$ROOTFS/usr/local/share/deshab/python"
if [ -f "$HELLO_SRC" ]; then
    install -m 0644 "$HELLO_SRC" "$ROOTFS/usr/local/share/deshab/python/hello.py"
fi

echo "[guest-python] $(chroot "$ROOTFS" /usr/bin/python3 --version)"
echo "[guest-python] hello.py -> $ROOTFS/usr/local/share/deshab/python/hello.py"
