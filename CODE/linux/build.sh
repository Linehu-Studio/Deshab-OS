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
    # 优先 kernel.org，失败则 fallback 清华镜像
    git clone --depth 1 --branch v6.6 \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git \
        "$LINUX_SRC" || {
        echo "[build] kernel.org failed, trying TUNA mirror..."
        git clone --depth 1 --branch v6.6 \
            https://mirrors.tuna.tsinghua.edu.cn/git/linux.git \
            "$LINUX_SRC"
    }
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
    INITRAMFS_TMP="$SCRIPT_DIR/initramfs.tmp"
    rm -rf "$INITRAMFS_TMP"
    mkdir -p "$INITRAMFS_TMP/bin"

    # Copy initramfs contents (init + bin/)
    cp -a "$INITRAMFS_DIR"/* "$INITRAMFS_TMP"/

    # 6a. Static busybox（缓存于 CODE/linux/.cache/，只下载一次）
    # busybox.net 官方预编译 x86_64 musl 静态二进制，含 sh/mount/uname 等 applet。
    BUSYBOX_CACHE="$SCRIPT_DIR/.cache/busybox"
    BUSYBOX_URL="https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
    if [ ! -x "$BUSYBOX_CACHE" ]; then
        echo "[build] Downloading static busybox..."
        mkdir -p "$(dirname "$BUSYBOX_CACHE")"
        curl -fL --retry 3 -o "$BUSYBOX_CACHE" "$BUSYBOX_URL" || \
            wget -O "$BUSYBOX_CACHE" "$BUSYBOX_URL"
        chmod +x "$BUSYBOX_CACHE"
    fi
    cp "$BUSYBOX_CACHE" "$INITRAMFS_TMP/bin/busybox"
    chmod +x "$INITRAMFS_TMP/bin/busybox"
    echo "[build]   busybox: $(du -h "$INITRAMFS_TMP/bin/busybox" | cut -f1)"

    # 6b. Build utsm_exec_daemon (static binary for initramfs)
    DAEMON_SRC="$INITRAMFS_DIR/bin/utsm_exec_daemon.c"
    if [ -f "$DAEMON_SRC" ]; then
        echo "[build] Compiling utsm_exec_daemon (static)..."
        cc -static -O2 -Wall -I"$PROJECT_ROOT/CODE/utsm-ipc" \
            -o "$INITRAMFS_TMP/bin/utsm_exec_daemon" "$DAEMON_SRC"
        strip "$INITRAMFS_TMP/bin/utsm_exec_daemon" 2>/dev/null || true
        echo "[build]   daemon: $(du -h "$INITRAMFS_TMP/bin/utsm_exec_daemon" | cut -f1)"
    fi

    # Create init script if not present
    if [ ! -f "$INITRAMFS_TMP/init" ]; then
        cat > "$INITRAMFS_TMP/init" <<'INITEOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "[utsm-linux] Boot complete"
exec /bin/sh
INITEOF
        chmod +x "$INITRAMFS_TMP/init"
    fi

    # 6c. 用内核自带 gen_init_cpio 打包（无需 root、无需 cpio 二进制）。
    # spec 文件声明目录/设备节点/文件/符号链接；设备节点元数据直接写入
    # cpio 归档，guest 内核 unpack 时即有 /dev/console 与 /dev/ttyS0。
    GEN_INIT_CPIO="$LINUX_SRC/usr/gen_init_cpio"
    if [ ! -x "$GEN_INIT_CPIO" ]; then
        echo "[build] Building gen_init_cpio host tool..."
        make usr/ >/dev/null
    fi

    SPEC="$SCRIPT_DIR/initramfs.tmp.spec"
    {
        echo "dir /bin 755 0 0"
        echo "dir /dev 755 0 0"
        echo "dir /proc 755 0 0"
        echo "dir /sys 755 0 0"
        # 控制台设备节点：kernel console=ttyS0 输出与 busybox sh 依赖
        echo "nod /dev/console 600 0 0 c 5 1"
        echo "nod /dev/null 666 0 0 c 1 3"
        echo "nod /dev/ttyS0 600 0 0 c 4 64"
        echo "file /init $INITRAMFS_TMP/init 755 0 0"
        echo "file /bin/busybox $INITRAMFS_TMP/bin/busybox 755 0 0"
        if [ -f "$INITRAMFS_TMP/bin/utsm_exec_daemon" ]; then
            echo "file /bin/utsm_exec_daemon $INITRAMFS_TMP/bin/utsm_exec_daemon 755 0 0"
        fi
        # busybox applet 符号链接（init 脚本与调试命令依赖）
        for app in sh mount umount echo sleep uname ls cat ps setsid cttyhack; do
            echo "slink /bin/$app /bin/busybox 777 0 0"
        done
    } > "$SPEC"

    "$GEN_INIT_CPIO" "$SPEC" | gzip -9 > "$OUTPUT_DIR/linux-initrd.img"
    echo "[build] initrd: $OUTPUT_DIR/linux-initrd.img ($(du -h $OUTPUT_DIR/linux-initrd.img | cut -f1))"
    rm -rf "$INITRAMFS_TMP" "$SPEC"
    cd "$LINUX_SRC"
fi

echo "[build] Done. Copy linux-bzImage (and linux-initrd.img) to your FAT32 image."
echo "[build] Limine will load them as boot modules for UTSM."

# 7. Enable Linux modules in limine.conf (uncomment the 4 commented lines)
LIMINE_CONF="$PROJECT_ROOT/SYSTEM/limine/limine.conf"
if [ -f "$LIMINE_CONF" ]; then
    if grep -q "^    # module_path: boot():/boot/linux-bzImage" "$LIMINE_CONF"; then
        echo "[build] Enabling Linux modules in limine.conf..."
        sed -i 's|^    # module_path: boot():/boot/linux-bzImage|    module_path: boot():/boot/linux-bzImage|' "$LIMINE_CONF"
        sed -i 's|^    # module_cmdline: linux:bzImage|    module_cmdline: linux:bzImage|' "$LIMINE_CONF"
        sed -i 's|^    # module_path: boot():/boot/linux-initrd.img|    module_path: boot():/boot/linux-initrd.img|' "$LIMINE_CONF"
        sed -i 's|^    # module_cmdline: linux:initrd|    module_cmdline: linux:initrd|' "$LIMINE_CONF"
        echo "[build]   Linux modules enabled in $LIMINE_CONF"
        echo "[build]   Run build.bat to rebuild the disk image with Linux modules"
    else
        echo "[build] limine.conf already has Linux modules enabled (or pattern changed)"
    fi
fi
