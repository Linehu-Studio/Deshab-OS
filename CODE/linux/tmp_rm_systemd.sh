#!/usr/bin/env bash
# Remove glibc-mismatched overlay libs (libsystemd) from the rootfs xorg-libs
# dir inside an image's ESP. cp -a injection never deletes, so stale files
# from earlier injections must be pruned explicitly.
set -Eeuo pipefail
for img in "$@"; do
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    mount "${L}p1" "$M"
    R=$(mktemp -d)
    mount -o loop "$M/boot/linux-rootfs.img" "$R"
    D="$R/usr/local/lib/xorg-libs"
    echo "$(basename "$img"): before=$(ls "$D" | wc -l)"
    rm -fv "$D"/libsystemd.so "$D"/libsystemd.so.0 "$D"/libsystemd.so.0.* 2>/dev/null || true
    sync
    echo "$(basename "$img"): after=$(ls "$D" | wc -l) systemd_left=$(ls "$D" | grep -c '^libsystemd.so' || true)"
    umount "$R"; rmdir "$R"
    umount "$M"; rmdir "$M"
    losetup -d "$L"
done