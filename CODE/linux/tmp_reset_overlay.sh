#!/usr/bin/env bash
# Reset the overlay dirs inside an image's rootfs so cp -a injection
# starts from a clean slate (cp -a never deletes stale files).
set -Eeuo pipefail
for img in "$@"; do
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    mount "${L}p1" "$M"
    R=$(mktemp -d)
    mount -o loop "$M/boot/linux-rootfs.img" "$R"
    X="$R/usr/local/lib/xorg-libs"
    P="$R/usr/local/lib/plasma-libs"
    echo "$(basename "$img"): before xorg=$(ls "$X" 2>/dev/null | wc -l) plasma=$(ls "$P" 2>/dev/null | wc -l)"
    rm -rf "$X" "$P"
    mkdir -p "$X" "$P"
    sync
    echo "$(basename "$img"): reset done"
    umount "$R"; rmdir "$R"
    umount "$M"; rmdir "$M"
    losetup -d "$L"
done