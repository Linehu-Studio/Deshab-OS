#!/usr/bin/env bash
# NOP-out ENDBR and clear IBT/SHSTK notes on guest ELF files inside an
# existing GPT image ESP (linux-rootfs.img + linux-extra-rootfs.img).
set -Eeuo pipefail

IMG="${1:-/mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img}"
ROOT="${2:-/mnt/d/Code/DEAICUP/Deshab}"
PY="$ROOT/CODE/linux/strip_endbr.py"

sed -i 's/\r$//' "$PY" 2>/dev/null || true

LOOP=$(losetup --find --show --partscan "$IMG")
sleep 0.3
ESP=$(mktemp -d)
mount "${LOOP}p1" "$ESP"

check_libc() {
    local m="$1"
    local libc=""
    for p in "$m/usr/lib/libc.so.6" "$m/lib/libc.so.6" "$m/usr/lib64/libc.so.6"; do
        if [[ -e "$p" ]]; then
            libc="$p"
            break
        fi
    done
    [[ -n "$libc" ]] || return 0
    python3 - "$libc" <<'PY'
import sys
path = sys.argv[1]
off = 0x199140
with open(path, "rb") as f:
    data = f.read()
chunk = data[off:off + 4] if len(data) > off + 4 else b""
print("[endbr] libc=%s size=%d @0x199140=%s" % (
    path, len(data), chunk.hex() if chunk else "short"))
if chunk == b"\xf3\x0f\x1e\xfa":
    sys.exit("ENDBR still present")
if chunk == b"\x0f\x1f\x40\x00":
    sys.exit("BADNOP [rax] still present")
PY
}

patch_img() {
    local img="$1"
    local m
    [[ -f "$img" ]] || return 0
    m=$(mktemp -d)
    if ! mount -o loop "$img" "$m"; then
        rmdir "$m"
        return 0
    fi
    echo "[endbr] scanning $img"
    # Prefer the real usr-merge trees; skip symlink aliases that would
    # double-walk the same files.
    local dirs=()
    if [[ -d "$m/usr/bin" ]]; then
        dirs+=(usr/bin usr/sbin usr/lib usr/lib64 usr/libexec usr/local/bin usr/local/lib)
    else
        dirs+=(bin lib lib64 usr/bin usr/sbin usr/lib usr/lib64 usr/libexec \
               usr/local/bin usr/local/lib)
    fi
    for d in "${dirs[@]}"; do
        if [[ -d "$m/$d" && ! -L "$m/$d" ]]; then
            python3 "$PY" "$m/$d" || echo "[endbr] WARN walk failed $m/$d"
        elif [[ -d "$m/$d" && -L "$m/$d" ]]; then
            echo "[endbr] skip symlink $d -> $(readlink "$m/$d")"
        fi
    done
    check_libc "$m" || echo "[endbr] WARN libc check failed on $img"
    sync
    umount "$m"
    rmdir "$m"
}

patch_img "$ESP/boot/linux-rootfs.img"
patch_img "$ESP/boot/linux-extra-rootfs.img"

umount "$ESP"
losetup -d "$LOOP"
rmdir "$ESP"
echo "[endbr] done $IMG"
