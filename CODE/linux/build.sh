#!/bin/bash
#
# Build Linux 6.6 LTS bzImage for UTSM dual-kernel guest.
#
# Prerequisites:
#   - Linux build environment (WSL, native Linux, or Docker)
#   - gcc, make, bc, flex, bison, libelf-dev, libssl-dev
#   - ~3GB free disk space
#
# Usage:
#   cd CODE/linux
#   ./build.sh
#
# Output:
#   - SYSTEM/boot/linux-bzImage  (kernel image)
#   - SYSTEM/boot/linux-initrd.img (minimal initramfs, if initramfs/ exists)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LINUX_SRC="${LINUX_SRC:-$SCRIPT_DIR/src}"
OUTPUT_DIR="$PROJECT_ROOT/SYSTEM/boot"
CONFIG_FILE="$SCRIPT_DIR/configs/utsm_x86_64_defconfig"
INITRAMFS_DIR="$SCRIPT_DIR/initramfs"

echo "[build] Linux kernel build for UTSM"
echo "[build] Project root: $PROJECT_ROOT"
echo "[build] Linux source: $LINUX_SRC"

# 1. Clone Linux 6.6 LTS if not present
if [ ! -d "$LINUX_SRC" ]; then
    echo "[build] Cloning Linux 6.6 LTS..."
    git clone --depth 1 --branch v6.6 \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git \
        "$LINUX_SRC"
fi

cd "$LINUX_SRC"

# 2. Apply UTSM patches if any
PATCH_DIR="$SCRIPT_DIR/patches"
if [ -d "$PATCH_DIR" ] && [ "$(ls -A $PATCH_DIR 2>/dev/null)" ]; then
    echo "[build] Applying UTSM patches..."

    # 2a. Apply .patch files (if any)
    for patch in "$PATCH_DIR"/*.patch; do
        if [ -f "$patch" ]; then
            echo "[build]   applying $(basename $patch)"
            git apply --check "$patch" 2>/dev/null && git apply "$patch" || \
                echo "[build]   WARNING: $patch already applied or failed"
        fi
    done

    # 2b. Install UTSM driver source files into drivers/utsm/
    UTSM_DRV_DIR="$LINUX_SRC/drivers/utsm"
    echo "[build]   installing UTSM driver into drivers/utsm/"
    mkdir -p "$UTSM_DRV_DIR/include"

    # Copy driver source, Kconfig, and Makefile
    cp "$PATCH_DIR/utsm_hcall.c" "$UTSM_DRV_DIR/utsm_hcall.c"
    cp "$PATCH_DIR/utsm_Kconfig" "$UTSM_DRV_DIR/Kconfig"
    cp "$PATCH_DIR/utsm_Makefile" "$UTSM_DRV_DIR/Makefile"

    # Copy shared IPC protocol header (included by both UTSM and Linux)
    cp "$PROJECT_ROOT/CODE/utsm-ipc/ipc_proto.h" "$UTSM_DRV_DIR/include/ipc_proto.h"

    # Add UTSM to drivers/Kconfig (if not already present)
    if ! grep -q "drivers/utsm/Kconfig" "$LINUX_SRC/drivers/Kconfig"; then
        echo '[build]   adding source "drivers/utsm/Kconfig" to drivers/Kconfig'
        echo 'source "drivers/utsm/Kconfig"' >> "$LINUX_SRC/drivers/Kconfig"
    fi

    # Add UTSM to drivers/Makefile (if not already present)
    if ! grep -q "CONFIG_UTSM_HCALL" "$LINUX_SRC/drivers/Makefile"; then
        echo '[build]   adding obj-$(CONFIG_UTSM_HCALL) += utsm/ to drivers/Makefile'
        echo 'obj-$(CONFIG_UTSM_HCALL) += utsm/' >> "$LINUX_SRC/drivers/Makefile"
    fi

    echo "[build]   UTSM driver installed:"
    echo "[build]     $UTSM_DRV_DIR/utsm_hcall.c"
    echo "[build]     $UTSM_DRV_DIR/Kconfig"
    echo "[build]     $UTSM_DRV_DIR/Makefile"
    echo "[build]     $UTSM_DRV_DIR/include/ipc_proto.h"
fi

# 3. Configure
echo "[build] Configuring kernel..."
cp "$CONFIG_FILE" .config
make olddefconfig

# 4. Build bzImage
echo "[build] Building bzImage (this may take 10-30 minutes)..."
make -j"$(nproc)" bzImage

# 5. Copy bzImage to SYSTEM/boot/
mkdir -p "$OUTPUT_DIR"
cp arch/x86/boot/bzImage "$OUTPUT_DIR/linux-bzImage"
echo "[build] bzImage: $OUTPUT_DIR/linux-bzImage ($(du -h $OUTPUT_DIR/linux-bzImage | cut -f1))"

# 6. Build initramfs if initramfs/ directory exists
if [ -d "$INITRAMFS_DIR" ]; then
    echo "[build] Building initramfs..."
    # Create a minimal initramfs with busybox or a simple init
    INITRAMFS_TMP="$SCRIPT_DIR/initramfs.tmp"
    rm -rf "$INITRAMFS_TMP"
    mkdir -p "$INITRAMFS_TMP"

    # Copy initramfs contents
    cp -a "$INITRAMFS_DIR"/* "$INITRAMFS_TMP"/

    # Create init script if not present
    if [ ! -f "$INITRAMFS_TMP/init" ]; then
        cat > "$INITRAMFS_TMP/init" <<'INITEOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "[utsm-linux] Boot complete"
echo "[utsm-linux] Console ready on /dev/hvc0 (virtio-console)"
exec /bin/sh
INITEOF
        chmod +x "$INITRAMFS_TMP/init"
    fi

    # Pack initramfs
    cd "$INITRAMFS_TMP"
    find . | cpio -H newc -o | gzip -9 > "$OUTPUT_DIR/linux-initrd.img"
    echo "[build] initrd: $OUTPUT_DIR/linux-initrd.img ($(du -h $OUTPUT_DIR/linux-initrd.img | cut -f1))"
    cd "$LINUX_SRC"
fi

echo "[build] Done. Copy linux-bzImage (and linux-initrd.img) to your FAT32 image."
echo "[build] Limine will load them as boot modules for UTSM."
