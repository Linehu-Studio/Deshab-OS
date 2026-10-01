#!/usr/bin/env bash
set -euo pipefail
LOG=/home/deshab/qemu_serial_loop1.log
clean() { tr -d '\r\0' < "$LOG"; }
echo "===== size ====="
wc -c "$LOG"
echo "===== GP/segfault/traps ====="
clean | grep -E 'general protection|segfault|traps:|endbr|#GP|SIGSEGV' | tail -n 50 || true
echo "===== KDE/exec/desktop ====="
clean | grep -E '\[deshab-kde\]|KDE |exec_async|exec rc|guest ping|desktop ready|serial 2|scanout|startplasma|Xorg|Xfbdev|Plasma|linux_compat|ibt=|shstk=|busybox|cmdline' | tail -n 80 || true
echo "===== vdc/persist ====="
clean | grep -E 'vdc|persist|overlay|EXEC_READY|switch_root' | tail -n 30 || true
