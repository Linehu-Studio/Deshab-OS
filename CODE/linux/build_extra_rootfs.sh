#!/bin/bash
#
# Build the persistent rw ext4 volume for the UTSM Linux guest.
#
# The default path creates the existing lightweight persistence volume.
# KDE_FULL=1 additionally pacstraps a self-contained Arch/KDE tree into the
# volume. The running guest keeps its current read-only root and can enter the
# KDE tree with /mnt/persist/usr/local/bin/deshab-kde-session. This keeps the
# native desktop and the guest console available as emergency fallbacks.
#
# Output:
#   SYSTEM/boot/linux-extra-rootfs.img
#
# Usage:
#   sudo bash CODE/linux/build_extra_rootfs.sh
#   sudo IMG_SIZE_MB=2048 bash CODE/linux/build_extra_rootfs.sh
#   sudo KDE_FULL=1 bash CODE/linux/build_extra_rootfs.sh   # 3900MB, FAT32-safe
#
# KDE_FULL=1 prerequisites:
#   - An Arch Linux build environment with arch-install-scripts (pacstrap)
#   - Working pacman mirrors and keyring
#   - Enough free space for the full KDE Applications suite

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="${EXTRA_ROOTFS_OUTPUT_DIR:-$PROJECT_ROOT/SYSTEM/boot}"
OUTPUT_IMG="${EXTRA_ROOTFS_OUTPUT:-$OUTPUT_DIR/linux-extra-rootfs.img}"

KDE_FULL="${KDE_FULL:-0}"
case "$KDE_FULL" in
    0|1) ;;
    *)
        echo "[extra-rootfs] ERROR: KDE_FULL must be 0 or 1" >&2
        exit 2
        ;;
esac

if [ -z "${IMG_SIZE_MB+x}" ]; then
    if [ "$KDE_FULL" -eq 1 ]; then
        # FAT32 ESP cannot store a file >= 4GiB. SYSTEM/boot is the only
        # pack input, so the extra-rootfs image must stay under that limit.
        IMG_SIZE_MB=3900
    else
        IMG_SIZE_MB=1024
    fi
fi
if ! [[ "$IMG_SIZE_MB" =~ ^[1-9][0-9]*$ ]]; then
    echo "[extra-rootfs] ERROR: IMG_SIZE_MB must be a positive integer" >&2
    exit 2
fi
if [ "$KDE_FULL" -eq 1 ] && [ "$IMG_SIZE_MB" -ge 4096 ]; then
    echo "[extra-rootfs] ERROR: IMG_SIZE_MB=$IMG_SIZE_MB >= 4GiB; FAT32 SYSTEM pack cannot store it" >&2
    exit 2
fi

WORK_DIR="${EXTRA_ROOTFS_WORK:-/root/extra_rootfs_work}"
MNT_DIR="$WORK_DIR/mnt"
NATIVE_IMG="$WORK_DIR/extra-rootfs.img"
KDE_PACSTRAP="${KDE_PACSTRAP:-pacstrap}"
KDE_PACMAN_CONF="${KDE_PACMAN_CONF:-}"
KDE_USER="${KDE_USER:-deshab}"
KDE_UID="${KDE_UID:-1000}"

if ! [[ "$KDE_USER" =~ ^[a-z_][a-z0-9_-]*$ ]]; then
    echo "[extra-rootfs] ERROR: KDE_USER is not a valid user name" >&2
    exit 2
fi
if ! [[ "$KDE_UID" =~ ^[1-9][0-9]*$ ]]; then
    echo "[extra-rootfs] ERROR: KDE_UID must be a positive integer" >&2
    exit 2
fi

# Explicit groups make review and future package-name updates straightforward.
# plasma-workspace now owns the Wayland session and replaces the obsolete
# plasma-wayland-session package on current Arch Linux.
KDE_DESKTOP_PACKAGES=(
    plasma-meta
    plasma-workspace
    sddm
)
KDE_APP_PACKAGES=(
    kde-applications-meta
)
KDE_WAYLAND_PACKAGES=(
    wayland
    wayland-protocols
    xorg-xwayland
    xdg-desktop-portal
    xdg-desktop-portal-kde
    libinput
    seatd
)
KDE_GRAPHICS_PACKAGES=(
    mesa
    mesa-utils
    libdrm
    libglvnd
    llvm-libs
    vulkan-icd-loader
    vulkan-swrast
)
KDE_SESSION_PACKAGES=(
    dbus
    dbus-broker
    systemd
    systemd-libs
    polkit
    pam
)
# noto-fonts-cjk is optional: the package is huge and Chinese mirrors
# often stall. Install later with KDE_EXTRA_PACKAGES="noto-fonts-cjk".
KDE_FONT_PACKAGES=(
    noto-fonts
    noto-fonts-emoji
    ttf-dejavu
)
KDE_PYTHON_PACKAGES=(
    python
)
KDE_BASE_PACKAGES=(
    base
    bash
    coreutils
    shadow
    util-linux
)
KDE_PACKAGES=(
    "${KDE_BASE_PACKAGES[@]}"
    "${KDE_DESKTOP_PACKAGES[@]}"
    "${KDE_WAYLAND_PACKAGES[@]}"
    "${KDE_GRAPHICS_PACKAGES[@]}"
    "${KDE_SESSION_PACKAGES[@]}"
    "${KDE_FONT_PACKAGES[@]}"
    "${KDE_PYTHON_PACKAGES[@]}"
)
if [ -n "${KDE_EXTRA_PACKAGES:-}" ]; then
    read -r -a KDE_EXTRA_PACKAGE_ARRAY <<< "$KDE_EXTRA_PACKAGES"
    KDE_PACKAGES+=("${KDE_EXTRA_PACKAGE_ARRAY[@]}")
fi

echo "[extra-rootfs] Building persistent rw volume"
echo "[extra-rootfs] Work dir: $WORK_DIR (native FS)"
echo "[extra-rootfs] Output:   $OUTPUT_IMG (${IMG_SIZE_MB}MB)"
echo "[extra-rootfs] KDE full: $KDE_FULL"

if [ "$(id -u)" -ne 0 ]; then
    echo "[extra-rootfs] ERROR: must run as root (loop mount required)" >&2
    exit 1
fi
# WSL/Ubuntu has no pacstrap. Fall back to an Arch bootstrap tarball
# extracted onto the native Linux disk, then chroot pacman.
KDE_BOOTSTRAP_TAR="${KDE_BOOTSTRAP_TAR:-$WORK_DIR/archlinux-bootstrap.tar.zst}"
KDE_USE_BOOTSTRAP=0
if [ "$KDE_FULL" -eq 1 ] && ! command -v "$KDE_PACSTRAP" >/dev/null 2>&1; then
    echo "[extra-rootfs] pacstrap not found; will use Arch bootstrap+chroot"
    KDE_USE_BOOTSTRAP=1
fi
if [ -n "$KDE_PACMAN_CONF" ] && [ ! -r "$KDE_PACMAN_CONF" ]; then
    echo "[extra-rootfs] ERROR: KDE_PACMAN_CONF is not readable: $KDE_PACMAN_CONF" >&2
    exit 1
fi

kde_download_bootstrap() {
    local url
    if [ -f "$KDE_BOOTSTRAP_TAR" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$KDE_BOOTSTRAP_TAR")"
    for url in \
        "https://mirrors.tuna.tsinghua.edu.cn/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" \
        "https://mirrors.ustc.edu.cn/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst" \
        "https://mirrors.aliyun.com/archlinux/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
    do
        echo "[extra-rootfs] Trying bootstrap: $url"
        if curl -fL --retry 3 --connect-timeout 20 -o "$KDE_BOOTSTRAP_TAR" "$url"; then
            return 0
        fi
        rm -f "$KDE_BOOTSTRAP_TAR"
    done
    echo "[extra-rootfs] ERROR: failed to download Arch bootstrap tarball" >&2
    return 1
}

kde_configure_pacman() {
    local root="$1"
    mkdir -p "$root/etc/pacman.d"
    cat > "$root/etc/pacman.d/mirrorlist" <<'MIRRORS'
Server = https://mirrors.tuna.tsinghua.edu.cn/archlinux/$repo/os/$arch
Server = https://mirrors.ustc.edu.cn/archlinux/$repo/os/$arch
Server = https://mirrors.aliyun.com/archlinux/$repo/os/$arch
Server = https://mirrors.cloud.tencent.com/archlinux/$repo/os/$arch
Server = https://geo.mirror.pkgbuild.com/$repo/os/$arch
Server = https://mirrors.kernel.org/archlinux/$repo/os/$arch
MIRRORS
    cat > "$root/etc/pacman.conf" <<'PACMANCONF'
[options]
HoldPkg = pacman glibc
Architecture = auto
SigLevel = Never
LocalFileSigLevel = Never
DisableDownloadTimeout
XferCommand = /usr/bin/curl -C - -fL --retry 8 --retry-delay 3 --connect-timeout 60 -o %o %u
[core]
Include = /etc/pacman.d/mirrorlist
[extra]
Include = /etc/pacman.d/mirrorlist
PACMANCONF
    mkdir -p "$root/tmp/pacman/lib/sync" "$root/tmp/pacman/cache/pkg"
    printf 'nameserver 1.1.1.1\n' > "$root/etc/resolv.conf"
}

kde_chroot_bind() {
    local root="$1"
    mkdir -p "$root"/{dev,proc,sys,run}
    mount --bind /dev "$root/dev"
    mount --bind /proc "$root/proc"
    mount --bind /sys "$root/sys"
    mount --bind /run "$root/run" 2>/dev/null || true
}

kde_unlock() {
    rm -f "$1/var/lib/pacman/db.lck" "$1/tmp/pacman/lib/db.lck"
}

kde_pacman_wave() {
    local root="$1"
    shift
    local attempt
    echo "[extra-rootfs] pacman wave: $*"
    for attempt in 1 2 3; do
        kde_unlock "$root"
        if chroot "$root" /usr/bin/pacman -S --noconfirm --needed --overwrite '*' "$@"; then
            return 0
        fi
        echo "[extra-rootfs] wave failed (attempt $attempt), retrying..."
        sleep 3
    done
    return 1
}

kde_install_full_suite() {
    local hello_src="$PROJECT_ROOT/SYSTEM/user/python/hello.py"
    if [ "$KDE_USE_BOOTSTRAP" -eq 0 ]; then
        echo "[extra-rootfs] Installing full Arch/KDE suite with pacstrap..."
        PACSTRAP_ARGS=(-K -c)
        if [ -n "$KDE_PACMAN_CONF" ]; then
            PACSTRAP_ARGS+=(-C "$KDE_PACMAN_CONF")
        fi
        "$KDE_PACSTRAP" "${PACSTRAP_ARGS[@]}" "$MNT_DIR" "${KDE_PACKAGES[@]}"
        if [ "${KDE_SKIP_APPS:-1}" != "1" ]; then
            echo "[extra-rootfs] Installing KDE applications meta (second pass)..."
            if ! chroot "$MNT_DIR" /usr/bin/pacman -S --noconfirm --needed "${KDE_APP_PACKAGES[@]}"; then
                echo "[extra-rootfs] WARNING: kde-applications-meta incomplete; Plasma/Wayland/Python remain"
            fi
        else
            echo "[extra-rootfs] skipping kde-applications-meta (KDE_SKIP_APPS=1, FAT32-safe)"
        fi
    else
        echo "[extra-rootfs] Installing Arch/KDE via bootstrap+chroot (native disk)..."
        kde_download_bootstrap
        if [ ! -x "$MNT_DIR/bin/bash" ] && [ ! -x "$MNT_DIR/usr/bin/bash" ]; then
            echo "[extra-rootfs] Extracting bootstrap into extra-rootfs..."
            tar --zstd -xf "$KDE_BOOTSTRAP_TAR" --strip-components=1 -C "$MNT_DIR"
        fi
        kde_configure_pacman "$MNT_DIR"
        kde_chroot_bind "$MNT_DIR"
        kde_unlock "$MNT_DIR"
        chroot "$MNT_DIR" /usr/bin/pacman -Sy --noconfirm
        if [ ! -x "$MNT_DIR/usr/bin/python3" ]; then
            kde_pacman_wave "$MNT_DIR" "${KDE_BASE_PACKAGES[@]}" "${KDE_PYTHON_PACKAGES[@]}"
        fi
        kde_pacman_wave "$MNT_DIR" "${KDE_WAYLAND_PACKAGES[@]}" "${KDE_GRAPHICS_PACKAGES[@]}" \
            || echo "[extra-rootfs] WARNING: wayland/graphics wave incomplete"
        kde_pacman_wave "$MNT_DIR" "${KDE_SESSION_PACKAGES[@]}" "${KDE_FONT_PACKAGES[@]}" \
            || echo "[extra-rootfs] WARNING: session/font wave incomplete"
        if [ ! -x "$MNT_DIR/usr/bin/plasmashell" ]; then
            kde_pacman_wave "$MNT_DIR" "${KDE_DESKTOP_PACKAGES[@]}" \
                || echo "[extra-rootfs] WARNING: Plasma desktop wave incomplete"
        fi
        if [ "${KDE_SKIP_APPS:-1}" != "1" ]; then
            echo "[extra-rootfs] Installing KDE applications meta (second pass)..."
            if ! kde_pacman_wave "$MNT_DIR" "${KDE_APP_PACKAGES[@]}"; then
                echo "[extra-rootfs] WARNING: kde-applications-meta incomplete; Plasma/Wayland/Python remain"
            fi
        else
            echo "[extra-rootfs] skipping kde-applications-meta (KDE_SKIP_APPS=1, FAT32-safe)"
        fi
        umount -R "$MNT_DIR/dev" 2>/dev/null || umount "$MNT_DIR/dev" 2>/dev/null || true
        umount "$MNT_DIR/proc" 2>/dev/null || true
        umount "$MNT_DIR/sys" 2>/dev/null || true
        umount "$MNT_DIR/run" 2>/dev/null || true
    fi
    if [ -f "$hello_src" ]; then
        mkdir -p "$MNT_DIR/usr/local/share/deshab/python"
        install -m 0644 "$hello_src" "$MNT_DIR/usr/local/share/deshab/python/hello.py"
        echo "[extra-rootfs] installed hello.py"
    fi
}

mkdir -p "$WORK_DIR" "$MNT_DIR" "$OUTPUT_DIR"

LOOP_DEV=""
MOUNTED=0
cleanup() {
    set +e
    umount -R "$MNT_DIR/dev" 2>/dev/null || umount "$MNT_DIR/dev" 2>/dev/null || true
    umount "$MNT_DIR/proc" 2>/dev/null || true
    umount "$MNT_DIR/sys" 2>/dev/null || true
    umount "$MNT_DIR/run" 2>/dev/null || true
    if [ "$MOUNTED" -eq 1 ]; then
        umount -R "$MNT_DIR" 2>/dev/null || umount "$MNT_DIR" 2>/dev/null || true
    fi
    if [ -n "$LOOP_DEV" ]; then
        losetup -d "$LOOP_DEV" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# 1. Allocate a sparse-friendly image and create its ext4 filesystem.
# EXTRA_ROOTFS_RESUME=1 reuses an existing native image (failed pacman retry).
if [ "${EXTRA_ROOTFS_RESUME:-0}" = "1" ] && [ -f "$NATIVE_IMG" ]; then
    echo "[extra-rootfs] resume: reusing $NATIVE_IMG"
else
    echo "[extra-rootfs] Allocating ${IMG_SIZE_MB}MB image..."
    rm -f "$NATIVE_IMG"
    dd if=/dev/zero of="$NATIVE_IMG" bs=1M count=0 seek="$IMG_SIZE_MB" status=none
    echo "[extra-rootfs] mkfs.ext4..."
    mkfs.ext4 -q -F -N 1048576 -O ^has_journal -L deshab-persist "$NATIVE_IMG"
fi

# 2. Mount and populate the lightweight persistence skeleton.
echo "[extra-rootfs] Populating directory skeleton..."
LOOP_DEV="$(losetup --find --show "$NATIVE_IMG")"
mount -t ext4 "$LOOP_DEV" "$MNT_DIR"
MOUNTED=1
rm -f "$MNT_DIR/var/lib/pacman/db.lck" "$MNT_DIR/tmp/pacman/lib/db.lck"
mkdir -p "$MNT_DIR/opt" "$MNT_DIR/home" "$MNT_DIR/root" "$MNT_DIR/vscode"
touch "$MNT_DIR/vscode/.keep"

if [ "$KDE_FULL" -eq 1 ]; then
    if [ "${KDE_FINALIZE_ONLY:-0}" = "1" ]; then
        echo "[extra-rootfs] finalize-only: keeping installed Plasma/Wayland/Python"
        hello_src="$PROJECT_ROOT/SYSTEM/user/python/hello.py"
        if [ -f "$hello_src" ]; then
            mkdir -p "$MNT_DIR/usr/local/share/deshab/python"
            install -m 0644 "$hello_src" "$MNT_DIR/usr/local/share/deshab/python/hello.py"
        fi
    else
        kde_install_full_suite
    fi

    # The image is a complete chroot kept separate from the active minimal
    # guest root. A marker lets the launcher distinguish wrapper and chroot
    # execution without relying on package presence in the outer root.
    mkdir -p "$MNT_DIR/etc" "$MNT_DIR/usr/local/bin" "$MNT_DIR/var/log"
    cat > "$MNT_DIR/etc/deshab-kde-root" <<'EOF'
Deshab Arch/KDE guest root
EOF
    : > "$MNT_DIR/etc/machine-id"

    if ! chroot "$MNT_DIR" /usr/bin/id "$KDE_USER" >/dev/null 2>&1; then
        chroot "$MNT_DIR" /usr/sbin/useradd \
            --create-home --uid "$KDE_UID" --shell /bin/bash "$KDE_USER"
    fi
    for group in audio input video wheel; do
        if chroot "$MNT_DIR" /usr/bin/getent group "$group" >/dev/null 2>&1; then
            chroot "$MNT_DIR" /usr/sbin/usermod --append --groups "$group" "$KDE_USER"
        fi
    done
    chroot "$MNT_DIR" /usr/bin/passwd --lock "$KDE_USER" >/dev/null

    # The current guest has a custom PID 1, so Plasma's supported legacy boot
    # path is selected. systemd and its user units remain installed for a
    # future systemd/logind boot path.
    mkdir -p "$MNT_DIR/home/$KDE_USER/.config"
    cat > "$MNT_DIR/home/$KDE_USER/.config/startkderc" <<'EOF'
[General]
systemdBoot=false
EOF
    chroot "$MNT_DIR" /usr/bin/chown -R "$KDE_USER:$KDE_USER" "/home/$KDE_USER"

    cat > "$MNT_DIR/usr/local/bin/deshab-kde-session" <<'SESSION_SCRIPT'
#!/bin/bash
set -Eeuo pipefail

KDE_ROOT="${DESHAB_KDE_ROOT:-/mnt/persist}"
SESSION_USER="${DESHAB_KDE_USER:-deshab}"
SOFTWARE_RENDERING="${DESHAB_KDE_SOFTWARE_RENDERING:-1}"
BOUND_MOUNTS=()
SEATD_PID=""

log() {
    printf '[deshab-kde] %s\n' "$*" | tee -a "$LOG_FILE" >&2
}

cleanup_session() {
    local i
    set +e
    if [ -n "$SEATD_PID" ]; then
        kill "$SEATD_PID" 2>/dev/null || true
        wait "$SEATD_PID" 2>/dev/null || true
    fi
    for ((i=${#BOUND_MOUNTS[@]} - 1; i >= 0; i--)); do
        umount -R "${BOUND_MOUNTS[$i]}" 2>/dev/null || \
            umount "${BOUND_MOUNTS[$i]}" 2>/dev/null || true
    done
}
trap cleanup_session EXIT INT TERM

if [ ! -e /etc/deshab-kde-root ]; then
    LOG_FILE="$KDE_ROOT/var/log/deshab-kde-session.log"
    if [ "$(id -u)" -ne 0 ]; then
        echo "[deshab-kde] ERROR: entering the KDE chroot requires root" >&2
        exit 1
    fi
    if [ ! -x "$KDE_ROOT/usr/bin/startplasma-wayland" ]; then
        echo "[deshab-kde] ERROR: no prepared KDE root at $KDE_ROOT" >&2
        exit 1
    fi

    mkdir -p "$KDE_ROOT"/{dev,proc,sys,run,var/log}
    touch "$LOG_FILE"
    log "entering KDE root at $KDE_ROOT"
    for fs in dev proc sys run; do
        target="$KDE_ROOT/$fs"
        if mountpoint -q "$target"; then
            continue
        fi
        if ! mount --rbind "/$fs" "$target"; then
            log "ERROR: could not bind /$fs into the KDE root"
            exit 1
        fi
        mount --make-rslave "$target" 2>/dev/null || true
        BOUND_MOUNTS+=("$target")
    done

    set +e
    chroot "$KDE_ROOT" /usr/bin/env \
        DESHAB_KDE_USER="$SESSION_USER" \
        DESHAB_KDE_SOFTWARE_RENDERING="$SOFTWARE_RENDERING" \
        /usr/local/bin/deshab-kde-session --inside
    status=$?
    set -e
    log "chroot session exited with status $status"
    exit "$status"
fi

LOG_FILE="/var/log/deshab-kde-session.log"
touch "$LOG_FILE"
if [ "$(id -u)" -ne 0 ]; then
    log "ERROR: session setup must start as root"
    exit 1
fi
if ! id "$SESSION_USER" >/dev/null 2>&1; then
    log "ERROR: session user does not exist: $SESSION_USER"
    exit 1
fi
if [ ! -e /dev/dri/card0 ]; then
    log "ERROR: /dev/dri/card0 is unavailable; leaving the fallback console intact"
    exit 1
fi

dbus-uuidgen --ensure=/etc/machine-id
session_uid="$(id -u "$SESSION_USER")"
session_gid="$(id -g "$SESSION_USER")"
runtime_dir="/run/user/$session_uid"
install -d -m 0700 -o "$session_uid" -g "$session_gid" "$runtime_dir"

if command -v seatd >/dev/null 2>&1 && [ ! -S /run/seatd.sock ]; then
    log "starting seatd for the non-systemd guest session"
    seatd -g video >>"$LOG_FILE" 2>&1 &
    SEATD_PID=$!
fi

if [ -x /usr/lib/plasma-dbus-run-session-if-needed ]; then
    SESSION_COMMAND=(
        /usr/lib/plasma-dbus-run-session-if-needed
        /usr/bin/startplasma-wayland
    )
elif command -v dbus-run-session >/dev/null 2>&1; then
    SESSION_COMMAND=(/usr/bin/dbus-run-session -- /usr/bin/startplasma-wayland)
else
    log "ERROR: no Plasma D-Bus session launcher is installed"
    exit 1
fi

SOFTWARE_ENV=()
if [ "$SOFTWARE_RENDERING" -eq 1 ]; then
    SOFTWARE_ENV=(
        LIBGL_ALWAYS_SOFTWARE=1
        GALLIUM_DRIVER=llvmpipe
        MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
        QT_QUICK_BACKEND=software
    )
    log "software rendering fallback enabled (llvmpipe/lavapipe)"
fi

log "starting Plasma Wayland as $SESSION_USER"
set +e
runuser --user "$SESSION_USER" -- /usr/bin/env \
    -u DBUS_SESSION_BUS_ADDRESS \
    HOME="/home/$SESSION_USER" \
    USER="$SESSION_USER" \
    LOGNAME="$SESSION_USER" \
    XDG_RUNTIME_DIR="$runtime_dir" \
    XDG_SESSION_TYPE=wayland \
    XDG_SESSION_CLASS=user \
    XDG_CURRENT_DESKTOP=KDE \
    DESKTOP_SESSION=plasma \
    KDE_FULL_SESSION=true \
    QT_QPA_PLATFORM=wayland \
    LIBSEAT_BACKEND=seatd \
    "${SOFTWARE_ENV[@]}" \
    "${SESSION_COMMAND[@]}" >>"$LOG_FILE" 2>&1
status=$?
set -e
if [ "$status" -ne 0 ]; then
    log "Plasma failed with status $status; see $LOG_FILE"
else
    log "Plasma session ended normally"
fi
exit "$status"
SESSION_SCRIPT
    chmod 0755 "$MNT_DIR/usr/local/bin/deshab-kde-session"

    # pacstrap -c uses the host cache; remove any target cache left by hooks.
    rm -rf "$MNT_DIR/var/cache/pacman/pkg/"*
fi

sync
umount "$MNT_DIR"
MOUNTED=0
losetup -d "$LOOP_DEV"
LOOP_DEV=""
trap - EXIT INT TERM

# 3. Copy only the final image back to the project tree.
echo "[extra-rootfs] Copying to $OUTPUT_IMG ..."
cp --sparse=always "$NATIVE_IMG" "$OUTPUT_IMG"

echo "[extra-rootfs] Done: $OUTPUT_IMG ($(du -h "$OUTPUT_IMG" | cut -f1) on disk)"
if [ "$KDE_FULL" -eq 1 ]; then
    echo "[extra-rootfs] Guest launcher:"
    echo "[extra-rootfs]   /mnt/persist/usr/local/bin/deshab-kde-session"
fi
echo "[extra-rootfs] Attach this image to the Linux guest through virtio-blk."
