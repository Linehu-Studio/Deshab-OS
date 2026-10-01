#!/usr/bin/env bash
# Inject KDE X11 session + shared Desktop icons into extra-rootfs (and rootfs).
set -Eeuo pipefail
ROOT="${1:-/mnt/d/Code/DEAICUP/Deshab}"
EXTRA="${2:-$ROOT/SYSTEM/boot/linux-extra-rootfs.img}"
ROOTFS="${3:-$ROOT/SYSTEM/boot/linux-rootfs.img}"
GUEST="$ROOT/CODE/linux/guest"
DESK="$ROOT/SYSTEM/user/desktop"

inject() {
    local img="$1"
    local m
    if [[ ! -f "$img" ]]; then
        echo "[kde-inject] skip missing $img"
        return 0
    fi
    m="$(mktemp -d)"
    if ! mount -o loop "$img" "$m"; then
        echo "[kde-inject] ERROR: mount $img" >&2
        rmdir "$m"
        return 1
    fi
    mkdir -p "$m/usr/local/bin" "$m/usr/local/share/deshab-desktop" \
             "$m/home/deshab/Desktop" "$m/var/log" "$m/etc"
    BUSYBOX="$ROOT/CODE/linux/.cache/busybox"
    if [[ -x "$BUSYBOX" ]]; then
        cp -f "$BUSYBOX" "$m/usr/local/bin/busybox.static"
        chmod 0755 "$m/usr/local/bin/busybox.static"
    fi
    sed 's/\r$//' "$GUEST/deshab-kde-session" > "$m/usr/local/bin/deshab-kde-session"
    sed 's/\r$//' "$GUEST/deshab-native-desktop" > "$m/usr/local/bin/deshab-native-desktop"
    chmod 0755 "$m/usr/local/bin/deshab-kde-session" \
               "$m/usr/local/bin/deshab-native-desktop"
    touch "$m/etc/deshab-kde-root"
    if [[ -d "$DESK" ]]; then
        cp -a "$DESK/." "$m/usr/local/share/deshab-desktop/"
        cp -a "$DESK/." "$m/home/deshab/Desktop/"
    fi
    if [[ -f "$m/usr/bin/startplasma-x11" ]]; then
        echo "[kde-inject] startplasma-x11 present in $img"
    elif [[ -f "$m/usr/bin/startplasma-wayland" ]]; then
        echo "[kde-inject] WARNING: only startplasma-wayland in $img"
    fi
    if command -v Xfbdev >/dev/null 2>&1; then
        :
    fi
    if [[ -x "$m/usr/bin/Xfbdev" || -x "$m/bin/Xfbdev" ]]; then
        echo "[kde-inject] Xfbdev present in $img"
    else
        echo "[kde-inject] Xfbdev not in $img (expected on linux-rootfs)"
    fi
    sync
    umount "$m"
    rmdir "$m"
    echo "[kde-inject] updated $img"
}

inject "$EXTRA"
inject "$ROOTFS"
