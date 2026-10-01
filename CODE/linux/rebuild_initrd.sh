#!/usr/bin/env bash
# Rebuild linux-initrd.img and optionally patch utsm_exec_daemon into
# the Arch rootfs image. Linux sources stay on the WSL native disk.
set -euo pipefail

REPO="${REPO:-/mnt/d/Code/DEAICUP/Deshab}"
LINUX_SRC="${LINUX_SRC:-/home/deshab/linux-6.6}"
OUT_DIR="$REPO/SYSTEM/boot"
INITRAMFS_DIR="$REPO/CODE/linux/initramfs"
DAEMON_SRC="$INITRAMFS_DIR/bin/utsm_exec_daemon.c"
INITRAMFS_TMP="${INITRAMFS_TMP:-/tmp/deshab-initrd}"
BUSYBOX_CACHE="$REPO/CODE/linux/.cache/busybox"

rm -rf "$INITRAMFS_TMP"
mkdir -p "$INITRAMFS_TMP/bin" "$OUT_DIR"

if [[ ! -x "$BUSYBOX_CACHE" ]]; then
    echo "[initrd] extracting busybox from existing initrd"
    mkdir -p "$(dirname "$BUSYBOX_CACHE")"
    XT=$(mktemp -d)
    gzip -dc "$OUT_DIR/linux-initrd.img" | (cd "$XT" && cpio -id --quiet 2>/dev/null) || true
    if [[ -x "$XT/bin/busybox" ]]; then
        cp "$XT/bin/busybox" "$BUSYBOX_CACHE"
        chmod +x "$BUSYBOX_CACHE"
    fi
    rm -rf "$XT"
fi
if [[ ! -x "$BUSYBOX_CACHE" ]]; then
    echo "[initrd] missing busybox at $BUSYBOX_CACHE" >&2
    exit 1
fi
cp "$BUSYBOX_CACHE" "$INITRAMFS_TMP/bin/busybox"
chmod +x "$INITRAMFS_TMP/bin/busybox"

echo "[initrd] compiling utsm_exec_daemon"
cc -static -O2 -Wall -I"$REPO/CODE/utsm-ipc" \
    -o "$INITRAMFS_TMP/bin/utsm_exec_daemon" "$DAEMON_SRC"
strip "$INITRAMFS_TMP/bin/utsm_exec_daemon" 2>/dev/null || true
mkdir -p "$REPO/.build_tmp"
cp -f "$INITRAMFS_TMP/bin/utsm_exec_daemon" "$REPO/.build_tmp/utsm_exec_daemon"

GEN_INIT_CPIO="$LINUX_SRC/usr/gen_init_cpio"
if [[ ! -x "$GEN_INIT_CPIO" ]]; then
    echo "[initrd] building gen_init_cpio"
    make -C "$LINUX_SRC" usr/ >/dev/null
fi

SPEC="$INITRAMFS_TMP/spec"
{
    echo "dir /bin 755 0 0"
    echo "dir /dev 755 0 0"
    echo "dir /proc 755 0 0"
    echo "dir /sys 755 0 0"
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "nod /dev/null 666 0 0 c 1 3"
    echo "nod /dev/ttyS0 600 0 0 c 4 64"
    # Windows checkouts often leave CRLF; a shebang with \\r will not exec.
    sed 's/\r$//' "$INITRAMFS_DIR/init" > "$INITRAMFS_TMP/init"
    chmod 0755 "$INITRAMFS_TMP/init"
    echo "file /init $INITRAMFS_TMP/init 755 0 0"
    echo "file /bin/busybox $INITRAMFS_TMP/bin/busybox 755 0 0"
    echo "file /bin/utsm_exec_daemon $INITRAMFS_TMP/bin/utsm_exec_daemon 755 0 0"
    for app in sh mount umount echo sleep uname ls cat ps mkdir switch_root setsid cttyhack test modprobe; do
        echo "slink /bin/$app /bin/busybox 777 0 0"
    done
} > "$SPEC"

"$GEN_INIT_CPIO" "$SPEC" | gzip -9 > "$OUT_DIR/linux-initrd.img"
echo "[initrd] $OUT_DIR/linux-initrd.img"
ls -lh "$OUT_DIR/linux-initrd.img"

ROOTFS="$OUT_DIR/linux-rootfs.img"
if [[ "${SKIP_ROOTFS_PATCH:-}" != "1" && -f "$ROOTFS" ]]; then
    M=$(mktemp -d)
    mount -o loop "$ROOTFS" "$M"
    if [[ -d "$M/usr/local/bin" ]]; then
        cp -f "$INITRAMFS_TMP/bin/utsm_exec_daemon" "$M/usr/local/bin/utsm_exec_daemon"
        chmod 0755 "$M/usr/local/bin/utsm_exec_daemon"
        echo "[initrd] patched daemon into linux-rootfs.img"
        # Arch glibc /bin/sh SIGSEGVs in this VMM. Host ping/Xfbdev helpers
        # go through /bin/sh; keep /bin/bash for scripts that need it.
        if [[ -x "$BUSYBOX_CACHE" ]]; then
            cp -f "$BUSYBOX_CACHE" "$M/bin/busybox.static"
            cp -f "$BUSYBOX_CACHE" "$M/usr/local/bin/busybox.static"
            chmod 0755 "$M/bin/busybox.static" "$M/usr/local/bin/busybox.static"
            # musl busybox has no SHSTK note; Arch bash/glibc does.
            # Keep /bin/sh -> bash so host ping and shebang scripts live.
            if [[ -e "$M/bin/bash" || -e "$M/usr/bin/bash" ]]; then
                ln -sfn bash "$M/bin/sh"
                echo "[initrd] /bin/sh -> bash"
            fi
        fi
        for s in deshab-kde-session deshab-native-desktop; do
            if [[ -f "$REPO/CODE/linux/guest/$s" ]]; then
                sed 's/\r$//' "$REPO/CODE/linux/guest/$s" > "$M/usr/local/bin/$s"
                chmod 0755 "$M/usr/local/bin/$s"
            fi
        done
    else
        echo "[initrd] rootfs has no /usr/local/bin, skip"
    fi
    umount "$M"
    rmdir "$M"
fi
