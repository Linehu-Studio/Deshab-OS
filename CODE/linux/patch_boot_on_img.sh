#!/usr/bin/env bash
# Copy updated ESP boot artifacts onto an existing GPT image without
# recopying the 4GiB extra-rootfs. Paths are relative to the FAT32 ESP.
set -Eeuo pipefail

IMG="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
ROOT="${2:-/mnt/d/Code/DEAICUP/Deshab}"
LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount "${LOOP}p1" "$M"

copy_if() {
    local src="$1" dst="$2"
    if [[ -f "$src" ]]; then
        mkdir -p "$(dirname "$dst")"
        cp -f "$src" "$dst"
        echo "[patch] $src -> $dst"
    else
        echo "[patch] skip missing $src"
    fi
}

copy_if "$ROOT/SYSTEM/boot/linux-bzImage" "$M/boot/linux-bzImage"
copy_if "$ROOT/SYSTEM/boot/utsm.elf" "$M/boot/utsm.elf"
copy_if "$ROOT/SYSTEM/boot/linux-initrd.img" "$M/boot/linux-initrd.img"
copy_if "$ROOT/SYSTEM/limine/limine.conf" "$M/limine.conf"
if [[ -d "$M/limine" ]]; then
    copy_if "$ROOT/SYSTEM/limine/limine.conf" "$M/limine/limine.conf"
fi

DAEMON="${DAEMON:-$ROOT/.build_tmp/utsm_exec_daemon}"
BINDMOUNT="${BINDMOUNT:-$ROOT/.build_tmp/utsm_bindmount}"
UTSMRUN="${UTSMRUN:-$ROOT/.build_tmp/utsm_run}"
BUSYBOX="${BUSYBOX:-$ROOT/CODE/linux/.cache/busybox}"
GUEST="$ROOT/CODE/linux/guest"

inject_scripts() {
    local img="$1"
    local 
    [[ -f "$img" ]] || return 0
    r=$(mktemp -d)
    if mount -o loop "$img" "$r"; then
        mkdir -p "$r/usr/local/bin" "$r/etc" "$r/mnt/persist"
        if [[ -f "$DAEMON" ]]; then
            cp -f "$DAEMON" "$r/usr/local/bin/utsm_exec_daemon"
            chmod 0755 "$r/usr/local/bin/utsm_exec_daemon"
        fi
        if [[ -f "$BINDMOUNT" ]]; then
            cp -f "$BINDMOUNT" "$r/usr/local/bin/utsm_bindmount"
            chmod 0755 "$r/usr/local/bin/utsm_bindmount"
            echo "[patch] utsm_bindmount -> $img"
        fi
        if [[ -f "$UTSMRUN" ]]; then
            cp -f "$UTSMRUN" "$r/usr/local/bin/utsm_run"
            chmod 0755 "$r/usr/local/bin/utsm_run"
            echo "[patch] utsm_run -> $img"
        fi
        XORG_GOOD="${XORG_GOOD:-$ROOT/.build_tmp/Xorg.good}"
        XORG_LIBS="${XORG_LIBS:-$ROOT/.build_tmp/xorg-libs}"
        MODESET="${MODESET:-/home/deshab/modesetting_drv.so}"
        if [[ "$(basename "$img")" != *extra* ]]; then
            mkdir -p "$r/usr/local/lib/xorg-libs" "$r/usr/local/lib/xorg/modules/drivers"
            if [[ -f "$XORG_GOOD" ]]; then
                cp -f "$XORG_GOOD" "$r/usr/local/lib/Xorg.good"
                chmod 0755 "$r/usr/local/lib/Xorg.good"
                echo "[patch] Xorg.good -> $img"
            fi
            if [[ -d "$XORG_LIBS" ]]; then
                cp -a "$XORG_LIBS/." "$r/usr/local/lib/xorg-libs/"
                echo "[patch] xorg-libs $(ls "$r/usr/local/lib/xorg-libs" | wc -l) -> $img"
            fi
            PLASMA_LIBS="${PLASMA_LIBS:-$ROOT/.build_tmp/plasma-libs}"
            if [[ -d "$PLASMA_LIBS" ]]; then
                mkdir -p "$r/usr/local/lib/plasma-libs"
                cp -a "$PLASMA_LIBS/." "$r/usr/local/lib/plasma-libs/"
                echo "[patch] plasma-libs $(ls "$r/usr/local/lib/plasma-libs" | wc -l) -> $img"
            fi
            if [[ -f "$MODESET" ]]; then
                cp -f "$MODESET" "$r/usr/local/lib/xorg/modules/drivers/modesetting_drv.so"
                chmod 0755 "$r/usr/local/lib/xorg/modules/drivers/modesetting_drv.so"
                echo "[patch] modesetting_drv.so -> $img"
            fi
        fi
        if [[ -x "$BUSYBOX" ]]; then
            mkdir -p "$r/bin" "$r/usr/local/bin"
            cp -f "$BUSYBOX" "$r/bin/busybox.static"
            cp -f "$BUSYBOX" "$r/usr/local/bin/busybox.static"
            chmod 0755 "$r/bin/busybox.static" "$r/usr/local/bin/busybox.static"
            # musl busybox.static PUSH-faults at GVA 0 under leaked SHSTK.
            # Keep /bin/sh -> bash (glibc). IBT landing pads are skipped in
            # L1 when guest_fetch can see ENDBR/NOP90.
            if [[ "$(basename "$img")" != *extra* ]]; then
                if [[ -e "$r/bin/bash" || -e "$r/usr/bin/bash" ]]; then
                    ln -sfn bash "$r/bin/sh"
                    echo "[patch] /bin/sh -> bash"
                fi
            fi
        fi
        if [[ -f "$GUEST/deshab-kde-session" ]]; then
            sed 's/\r$//' "$GUEST/deshab-kde-session" > "$r/usr/local/bin/deshab-kde-session"
            chmod 0755 "$r/usr/local/bin/deshab-kde-session"
        fi
        if [[ -f "$GUEST/deshab-native-desktop" ]]; then
            sed 's/\r$//' "$GUEST/deshab-native-desktop" > "$r/usr/local/bin/deshab-native-desktop"
            chmod 0755 "$r/usr/local/bin/deshab-native-desktop"
        fi
        if [[ "$(basename "$img")" == *extra* ]]; then
            touch "$r/etc/deshab-kde-root"
            chmod 0755 "$r/dev" "$r/proc" "$r/sys" "$r/run" 2>/dev/null || true
        fi
        echo "[patch] guest scripts -> $img"
        umount "$r"
    fi
    rmdir "$r"
}

inject_scripts "$M/boot/linux-rootfs.img"
# Do not loop-mount or rewrite linux-extra-rootfs.img. Guest maps that
# file as vdc; host writes plus virtio T_OUT have produced runtime
# ext4 checksums even when e2fsck of the image is clean. The session
# copies itself onto the persist overlay before chroot.
# KDE wrapper on Arch rootfs is enough for the first hop; --inside uses
# the overlay copy after chroot.
copy_if "$ROOT/SYSTEM/system/deshab64/deshab.elf" "$M/system/deshab64/deshab.elf"
copy_if "$ROOT/SYSTEM/system/deshab64/desktop.elf" "$M/system/deshab64/desktop.elf"
copy_if "$ROOT/SYSTEM/system/deshab64/FUCK" "$M/system/deshab64/FUCK"
copy_if "$ROOT/SYSTEM/LINUXAPP.CNF" "$M/LINUXAPP.CNF"
copy_if "$ROOT/SYSTEM/user/desktop/kde.lnk" "$M/user/desktop/kde.lnk"
copy_if "$ROOT/SYSTEM/user/desktop/kde.desktop" "$M/user/desktop/kde.desktop"

sync
umount "$M"
losetup -d "$LOOP"
rmdir "$M"
echo "[patch] updated $IMG ESP boot/system files"
