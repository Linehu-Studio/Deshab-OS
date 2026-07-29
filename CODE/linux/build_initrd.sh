#!/bin/bash
# Build initramfs only (assumes bzImage already built)
set -e

SCRIPT_DIR="/mnt/d/Code/Deshab/CODE/linux"
PROJECT_ROOT="/mnt/d/Code/Deshab"
OUTPUT_DIR="$PROJECT_ROOT/SYSTEM/boot"
INITRAMFS_DIR="$SCRIPT_DIR/initramfs"
LINUX_SRC="$HOME/linux-6.6"
INITRAMFS_TMP="$SCRIPT_DIR/initramfs.tmp"

rm -rf "$INITRAMFS_TMP"
mkdir -p "$INITRAMFS_TMP/bin"
cp -a "$INITRAMFS_DIR"/* "$INITRAMFS_TMP"/

# Download static busybox
BUSYBOX_CACHE="$SCRIPT_DIR/.cache/busybox"
BUSYBOX_URL="https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
if [ ! -x "$BUSYBOX_CACHE" ]; then
    echo "[initrd] Downloading static busybox..."
    mkdir -p "$(dirname "$BUSYBOX_CACHE")"
    curl -fL --retry 3 -o "$BUSYBOX_CACHE" "$BUSYBOX_URL"
    chmod +x "$BUSYBOX_CACHE"
fi
cp "$BUSYBOX_CACHE" "$INITRAMFS_TMP/bin/busybox"
chmod +x "$INITRAMFS_TMP/bin/busybox"
echo "[initrd] busybox copied"

# Build utsm_exec_daemon
DAEMON_SRC="$INITRAMFS_DIR/bin/utsm_exec_daemon.c"
if [ -f "$DAEMON_SRC" ]; then
    echo "[initrd] Compiling utsm_exec_daemon..."
    cc -static -O2 -Wall "-I$PROJECT_ROOT/CODE/utsm-ipc" \
        -o "$INITRAMFS_TMP/bin/utsm_exec_daemon" "$DAEMON_SRC"
    strip "$INITRAMFS_TMP/bin/utsm_exec_daemon" 2>/dev/null || true
    echo "[initrd] daemon compiled"
fi

# gen_init_cpio
GEN_INIT_CPIO="$LINUX_SRC/usr/gen_init_cpio"
if [ ! -x "$GEN_INIT_CPIO" ]; then
    echo "[initrd] Building gen_init_cpio..."
    cd "$LINUX_SRC" && make usr/ >/dev/null
fi

SPEC="$SCRIPT_DIR/initramfs.tmp.spec"
{
    echo "dir /bin 755 0 0"
    echo "dir /dev 755 0 0"
    echo "dir /proc 755 0 0"
    echo "dir /sys 755 0 0"
    echo "nod /dev/console 600 0 0 c 5 1"
    echo "nod /dev/null 666 0 0 c 1 3"
    echo "nod /dev/ttyS0 600 0 0 c 4 64"
    echo "file /init $INITRAMFS_TMP/init 755 0 0"
    echo "file /bin/busybox $INITRAMFS_TMP/bin/busybox 755 0 0"
    if [ -f "$INITRAMFS_TMP/bin/utsm_exec_daemon" ]; then
        echo "file /bin/utsm_exec_daemon $INITRAMFS_TMP/bin/utsm_exec_daemon 755 0 0"
    fi
    for app in sh mount umount echo sleep uname ls cat ps setsid cttyhack; do
        echo "slink /bin/$app /bin/busybox 777 0 0"
    done
} > "$SPEC"

"$GEN_INIT_CPIO" "$SPEC" | gzip -9 > "$OUTPUT_DIR/linux-initrd.img"
echo "[initrd] Done: $OUTPUT_DIR/linux-initrd.img"
ls -lh "$OUTPUT_DIR/linux-initrd.img"
rm -rf "$INITRAMFS_TMP" "$SPEC"
