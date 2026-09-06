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
#   sudo KDE_FULL=1 IMG_SIZE_MB=8192 bash CODE/linux/build_extra_rootfs.sh
#
# KDE_FULL=1 prerequisites:
#   - An Arch Linux build environment with arch-install-scripts (pacstrap)
#   - Working pacman mirrors and keyring
#   - Enough free space for the full KDE Applications suite

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$PROJECT_ROOT/SYSTEM/boot"
OUTPUT_IMG="$OUTPUT_DIR/linux-extra-rootfs.img"

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
        IMG_SIZE_MB=8192
    else
        IMG_SIZE_MB=1024
    fi
fi
if ! [[ "$IMG_SIZE_MB" =~ ^[1-9][0-9]*$ ]]; then
    echo "[extra-rootfs] ERROR: IMG_SIZE_MB must be a positive integer" >&2
    exit 2
fi
if [ "$KDE_FULL" -eq 1 ] && [ "$IMG_SIZE_MB" -lt 8192 ]; then
    echo "[extra-rootfs] WARNING: full KDE is expected to need at least 8192MB" >&2
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
    kde-applications-meta
    plasma-workspace
    sddm
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
KDE_FONT_PACKAGES=(
    noto-fonts
    noto-fonts-cjk
    noto-fonts-emoji
    ttf-dejavu
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
if [ "$KDE_FULL" -eq 1 ] && ! command -v "$KDE_PACSTRAP" >/dev/null 2>&1; then
    echo "[extra-rootfs] ERROR: KDE_FULL=1 requires pacstrap" >&2
    echo "[extra-rootfs] Install arch-install-scripts in an Arch build environment," >&2
    echo "[extra-rootfs] or set KDE_PACSTRAP to its absolute path." >&2
    exit 1
fi
if [ -n "$KDE_PACMAN_CONF" ] && [ ! -r "$KDE_PACMAN_CONF" ]; then
    echo "[extra-rootfs] ERROR: KDE_PACMAN_CONF is not readable: $KDE_PACMAN_CONF" >&2
    exit 1
fi

mkdir -p "$WORK_DIR" "$MNT_DIR" "$OUTPUT_DIR"

LOOP_DEV=""
MOUNTED=0
cleanup() {
    set +e
    if [ "$MOUNTED" -eq 1 ]; then
        umount -R "$MNT_DIR" 2>/dev/null || umount "$MNT_DIR" 2>/dev/null || true
    fi
    if [ -n "$LOOP_DEV" ]; then
        losetup -d "$LOOP_DEV" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# 1. Allocate a sparse-friendly image and create its ext4 filesystem.
echo "[extra-rootfs] Allocating ${IMG_SIZE_MB}MB image..."
rm -f "$NATIVE_IMG"
dd if=/dev/zero of="$NATIVE_IMG" bs=1M count=0 seek="$IMG_SIZE_MB" status=none

echo "[extra-rootfs] mkfs.ext4..."
mkfs.ext4 -q -F -N 1048576 -O ^has_journal -L deshab-persist "$NATIVE_IMG"

# 2. Mount and populate the lightweight persistence skeleton.
echo "[extra-rootfs] Populating directory skeleton..."
LOOP_DEV="$(losetup --find --show "$NATIVE_IMG")"
mount -t ext4 "$LOOP_DEV" "$MNT_DIR"
MOUNTED=1
mkdir -p "$MNT_DIR/opt" "$MNT_DIR/home" "$MNT_DIR/root" "$MNT_DIR/vscode"
touch "$MNT_DIR/vscode/.keep"

if [ "$KDE_FULL" -eq 1 ]; then
    echo "[extra-rootfs] Installing full Arch/KDE suite with pacstrap..."
    PACSTRAP_ARGS=(-K -c)
    if [ -n "$KDE_PACMAN_CONF" ]; then
        PACSTRAP_ARGS+=(-C "$KDE_PACMAN_CONF")
    fi
    "$KDE_PACSTRAP" "${PACSTRAP_ARGS[@]}" "$MNT_DIR" "${KDE_PACKAGES[@]}"

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
