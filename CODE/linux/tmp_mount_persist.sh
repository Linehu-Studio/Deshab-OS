#!/usr/bin/env bash
# Mount the GPT image partitions read-only on the host and report the KDE payload layout.
# Goal: decide whether the "truncated / smashed .so" symptom seen in the guest is present
# in the on-disk image (build-time corruption) or only appears at guest runtime (block path).
# Usage: bash tmp_mount_persist.sh [image]
set -u
IMG=${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}
P1=/mnt/deshab_p1
P2=/mnt/deshab_p2
umount -q "$P1" 2>/dev/null
umount -q "$P2" 2>/dev/null
mkdir -p "$P1" "$P2"
L=$(losetup --partscan --find --show "$IMG")
echo "=== loop $L"
mount -o ro "${L}p1" "$P1" || { echo "mount p1 failed"; exit 1; }
mount -o ro "${L}p2" "$P2" || { echo "mount p2 failed"; exit 1; }

echo "=== p1 top"
ls -la "$P1"
echo "=== p1 nested images"
ls -la "$P1/boot" 2>/dev/null
echo "=== p2 top"
ls -la "$P2"
echo "=== p2 usr"
ls -la "$P2/usr" 2>/dev/null | head -30
echo "=== p2 lib dirs"
ls -la "$P2/usr/lib" 2>/dev/null | head -20
ls -la "$P2/usr/lib64" 2>/dev/null | head -20
echo "=== p2 bin"
ls -la "$P2/usr/bin" 2>/dev/null | head -60
echo "=== df"
df -h "$P1" "$P2"