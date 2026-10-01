#!/usr/bin/env bash
# Push the updated FUCK config (sched demo_ms=0) into the ESP of the given
# images. ESP path is /system/deshab64/FUCK. Readback verified by md5.
set -Eeuo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
SRC="$ROOT/SYSTEM/system/deshab64/FUCK"
[[ -f "$SRC" ]] || { echo "missing $SRC"; exit 2; }
EXP=$(md5sum "$SRC" | cut -d' ' -f1)
echo "source FUCK md5=$EXP"
rc=0
for img in "$@"; do
    L=$(losetup --find --show --partscan "$img")
    sleep 0.3
    M=$(mktemp -d)
    if ! mount "${L}p1" "$M" 2>/dev/null; then
        echo "$(basename "$img"): mount p1 FAILED"; rmdir "$M"; losetup -d "$L"; rc=1; continue
    fi
    mkdir -p "$M/system/deshab64"
    cp -f "$SRC" "$M/system/deshab64/FUCK"
    sync
    got=$(md5sum "$M/system/deshab64/FUCK" | cut -d' ' -f1)
    [[ "$got" == "$EXP" ]] && echo "$(basename "$img"): FUCK APPLIED" || { echo "$(basename "$img"): VERIFY FAILED"; rc=1; }
    umount "$M"; rmdir "$M"; losetup -d "$L"
done
exit $rc