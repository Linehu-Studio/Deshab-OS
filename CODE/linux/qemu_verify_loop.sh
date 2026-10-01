#!/usr/bin/env bash
# Headless QEMU verify: Linux guest park + native desktop + KDE scanout.
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMG="${IMG_OVERRIDE:-$ROOT/ISO/deshab-dev.img}"
LOG="${QEMU_SERIAL_LOG:-/home/deshab/qemu_serial_loop1.log}"
MON="${QEMU_MONITOR:-telnet:127.0.0.1:45454,server,nowait}"
TIMEOUT="${QEMU_VERIFY_TIMEOUT:-480}"
SEND_KDE="${QEMU_SEND_KDE:-1}"
SERIAL_TCP="${QEMU_SERIAL_TCP:-127.0.0.1:45455}"

rm -f "$LOG"
: > "$LOG"

export IMG_OVERRIDE="$IMG"
export QEMU_SERIAL_LOG="$LOG"
export QEMU_DISPLAY=none
export QEMU_MONITOR="$MON"
export QEMU_SERIAL_TCP="$SERIAL_TCP"
# Do not attach the AHCI SATA *sidecar*. The boot disk is already AHCI
# (ISO/deshab-dev.img). The old sata_fat32_dsk.img hid ESP deshab.elf.
export SATA_IMG_OVERRIDE="/tmp/deshab-verify-no-sata"
export NVME_IMG_OVERRIDE="/tmp/deshab-verify-no-nvme"
export USB_IMG_OVERRIDE="/tmp/deshab-verify-no-usb"

echo "[verify] starting QEMU, log=$LOG timeout=${TIMEOUT}s serial_tcp=$SERIAL_TCP"
cd "$ROOT/ISO"
# Strip CRLF if the script was edited on Windows.
sed -i 's/\r$//' ./run_qemu_kvm.sh 2>/dev/null || true
bash ./run_qemu_kvm.sh &
QEMU_PID=$!

cleanup() {
    set +e
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

log_text() {
    # Serial logs often contain CR / NUL; strip so grep anchors work.
    tr -d '\r\0' < "$LOG" 2>/dev/null || true
}

log_has() {
    log_text | grep -qE "$1"
}

send_serial() {
    local msg="$1"
    local host="${SERIAL_TCP%%:*}"
    local port="${SERIAL_TCP##*:}"
    python3 - "$host" "$port" "$msg" <<'PY' || true
import socket, sys
host, port, msg = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.create_connection((host, port), 3)
s.sendall(msg.encode())
s.close()
PY
}

sent_kde=0
dsk_started=0
plasma_painted=0
deadline=$((SECONDS + TIMEOUT))
guest_ok=0
session_ok=0
kde_ok=0
desktop_ready=0

while kill -0 "$QEMU_PID" 2>/dev/null && [[ $SECONDS -lt $deadline ]]; do
    if log_has 'EXEC_READY received from /dev/utsm' && \
       log_has 'guest parked \(daemon ready\)'; then
        guest_ok=1
    fi
    if log_has '\[DSK\] session=(kde|native)$' || \
       log_has '\[DSK\] session=ask, waiting for 1/2'; then
        session_ok=1
    fi
    if log_has 'desktop ready'; then
        desktop_ready=1
    fi
    if log_has 'session=ask, waiting for 1/2'; then
        if [[ "$SEND_KDE" == "1" && "$sent_kde" == "0" ]]; then
            sleep 1
            echo "[verify] sending serial 2 for KDE (chooser)"
            send_serial '2'
            sent_kde=1
        fi
    fi
    if [[ "$desktop_ready" == "1" && "$SEND_KDE" == "1" && "$sent_kde" == "0" ]]; then
        echo "[verify] sending serial 2 for KDE (native desktop)"
        send_serial '2'
        sent_kde=1
        deadline=$((SECONDS + 180))
    fi
    # "Plasma failed" is sometimes dropped by kmsg. The loader error
    # and the chroot exit status are the ones that actually reach serial.
    if log_has '\[deshab-kde\] Plasma failed|invalid ELF header|error while loading shared libraries|exited with status [1-9]|fatal signal'; then
        kde_ok=3
    elif log_has '\[deshab-kde\] starting Plasma'; then
        kde_ok=1
    elif [[ "$kde_ok" != "1" ]] && \
         log_has 'KDE preflight missing|KDE scanout timeout|KDE failed|could not bind|chroot session exited'; then
        kde_ok=3
    elif [[ "$kde_ok" == "0" ]] && \
         log_has 'serial 2: launch KDE|kde: launching fullscreen Plasma|KDE host loop|KDE spawned|\[deshab-kde\] interpreter alive|\[deshab-kde\] entering KDE'; then
        kde_ok=2
    fi
    # P9: Plasma 只活着还不够，必须真的推帧。Xorg/modesetting 每次提交
    # 都发 RESOURCE_FLUSH；utsm.elf 里加了 FLUSH_N 计数日志（每 64 次一行）。
    # starting Plasma 之后出现 FLUSH_N 才算 Plasma 画了。
    if [[ "$kde_ok" == "1" ]]; then
        if log_has '\[VGPU\] FLUSH_N'; then
            plasma_painted=1
        fi
    fi
    # drm fbdev SET_SCANOUT at boot is not Plasma. "entering KDE" only
    # means the session reached the chroot setup, not that Plasma painted.
    if [[ "$kde_ok" == "1" ]] && [[ "$guest_ok" == "1" ]]; then
        if [[ "$sent_kde" == "1" ]]; then
            sleep 60
        else
            sleep 8
        fi
        # starting Plasma is logged before the child returns. Recheck
        # after the wait so a 127 during this window is not a false OK.
        if log_has '\[deshab-kde\] Plasma failed|invalid ELF header|error while loading shared libraries|exited with status [1-9]|fatal signal'; then
            kde_ok=3
        elif log_has '\[VGPU\] FLUSH_N'; then
            plasma_painted=1
            break
        else
            break
        fi
    fi
    if [[ "$guest_ok" == "1" && "$session_ok" == "1" && "$kde_ok" == "3" ]]; then
        sleep 20
        break
    fi
    if [[ "$dsk_started" == "0" ]] && log_has '\[DSK\] FUCK config loaded'; then
        dsk_started=1
        deadline=$((SECONDS + 240))
        echo "[verify] DSK started, extending wait 240s"
    fi
    sleep 2
    echo "[verify] t=${SECONDS}s log=$(wc -c < "$LOG" 2>/dev/null || echo 0) guest=$guest_ok session=$session_ok desktop=$desktop_ready kde=$kde_ok"
done

echo "===== VERIFY SUMMARY ====="
echo "guest_ok=$guest_ok session_ok=$session_ok kde_ok=$kde_ok sent_kde=$sent_kde desktop_ready=$desktop_ready plasma_painted=$plasma_painted"
echo "----- markers -----"
log_text | grep -E 'EXEC_READY|guest parked|session=ask|session=kde|session=native|KDE |Linux guest|FUCK config loaded|switch_root|utsm-linux|desktop ready|serial 2|FAT32 volume LBA|desktop load|user/desktop|VGPU|scanout|FLUSH|deshab-kde|vdc|persist|segfault|fatal signal|Xorg.0.log|Fatal server|overlay' | tail -n 120 || true

if [[ "$guest_ok" != "1" || "$session_ok" != "1" ]]; then
    echo VERIFY_FAIL
    exit 1
fi
if [[ "$kde_ok" == "1" ]]; then
    if [[ "$plasma_painted" == "1" ]]; then
        echo VERIFY_KDE_PLASMA_OK
        exit 0
    else
        echo VERIFY_KDE_ALIVE_NO_FRAME
        exit 1
    fi
fi
if [[ "$kde_ok" == "2" ]]; then
    echo VERIFY_KDE_SPAWNED
    exit 1
fi
echo VERIFY_PARTIAL_OK
exit 1
