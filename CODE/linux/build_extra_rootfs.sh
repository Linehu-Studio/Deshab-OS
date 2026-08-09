#!/bin/bash
#
# Build the persistent rw ext4 volume for the UTSM Linux guest (VSCode Phase 4).
#
# Produces a sparse-friendly ext4 image exposed to the guest as a WRITABLE
# virtio-blk device (/dev/vdc, slot6 @ GPA 0xF4006000 IRQ11) by the UTSM
# extra_rootfs backend. The Arch /sbin/init mounts it at /mnt/persist and
# bind-mounts its subtrees over /opt, /home and /root, giving VSCode and
# user data a persistent home across the read-only rootfs.
#
# Output:
#   SYSTEM/boot/linux-extra-rootfs.img  (ext4 image, default 1024MB)
#
# After generating, uncomment the two extra-rootfs module lines in
# SYSTEM/limine/limine.conf so Limine preloads it as a boot module.
#
# Prerequisites:
#   - WSL or native Linux with root access
#   - mkfs.ext4, losetup, mount (or debugfs for unprivileged population)
#
# Usage:
#   sudo bash CODE/linux/build_extra_rootfs.sh            # 1GB default
#   sudo IMG_SIZE_MB=2048 bash CODE/linux/build_extra_rootfs.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/SYSTEM/boot"
OUTPUT_IMG="$OUTPUT_DIR/linux-extra-rootfs.img"
IMG_SIZE_MB=${IMG_SIZE_MB:-1024}

WORK_DIR="${EXTRA_ROOTFS_WORK:-/root/extra_rootfs_work}"
MNT_DIR="$WORK_DIR/mnt"

echo "[extra-rootfs] Building persistent rw volume"
echo "[extra-rootfs] Work dir: $WORK_DIR (native FS)"
echo "[extra-rootfs] Output:   $OUTPUT_IMG (${IMG_SIZE_MB}MB)"

mkdir -p "$WORK_DIR" "$MNT_DIR" "$OUTPUT_DIR"

if [ "$(id -u)" -ne 0 ]; then
    echo "[extra-rootfs] ERROR: must run as root (loop mount required)" >&2
    exit 1
fi

# 1. Allocate the image file (seek-truncate: sparse on supporting filesystems)
echo "[extra-rootfs] Allocating ${IMG_SIZE_MB}MB image..."
dd if=/dev/zero of="$WORK_DIR/extra-rootfs.img" bs=1M count=0 seek="$IMG_SIZE_MB" status=none

# 2. Create the ext4 filesystem.
#    -N 1048576 caps inode count (1M inodes is plenty for VSCode + user data
#    and keeps the inode table small); -O ^has_journal skips the journal —
#    the volume is memory-backed (writes are volatile until Phase 7 writeback),
#    so journal overhead buys nothing.
echo "[extra-rootfs] mkfs.ext4..."
mkfs.ext4 -q -F -N 1048576 -O ^has_journal -L deshab-persist "$WORK_DIR/extra-rootfs.img"

# 3. Populate the base directory skeleton via loop mount.
echo "[extra-rootfs] Populating directory skeleton..."
LOOP_DEV="$(losetup --find --show "$WORK_DIR/extra-rootfs.img")"
trap 'losetup -d "$LOOP_DEV" 2>/dev/null || true' EXIT

mount -t ext4 "$LOOP_DEV" "$MNT_DIR"
mkdir -p "$MNT_DIR/opt" "$MNT_DIR/home" "$MNT_DIR/root" "$MNT_DIR/vscode"
# /mnt/persist/vscode will hold the VSCode tarball extraction target marker;
# /opt bind-mount covers /opt/vscode at runtime.
touch "$MNT_DIR/vscode/.keep"
sync
umount "$MNT_DIR"
losetup -d "$LOOP_DEV"
trap - EXIT

# 4. Copy the final image to the Windows side (SYSTEM/boot).
echo "[extra-rootfs] Copying to $OUTPUT_IMG ..."
cp "$WORK_DIR/extra-rootfs.img" "$OUTPUT_IMG"

echo "[extra-rootfs] Done: $OUTPUT_IMG ($(du -h "$OUTPUT_IMG" | cut -f1) on disk)"
echo "[extra-rootfs] Next: uncomment the extra-rootfs module lines in"
echo "[extra-rootfs]   SYSTEM/limine/limine.conf, then rebuild the ESP image."
