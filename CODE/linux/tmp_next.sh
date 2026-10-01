#!/usr/bin/env bash
# Rebuild initrd, probe extra-rootfs without drvfs loop-mount, inject, verify.
set -euo pipefail
ROOT=/mnt/d/Code/DEAICUP/Deshab
export REPO="$ROOT"
killall qemu-system-x86_64 2>/dev/null || true
sleep 1

sed -i 's/\r$//' \
    "$ROOT/CODE/linux/rebuild_initrd.sh" \
    "$ROOT/CODE/linux/inject_boot_and_busybox.sh" \
    "$ROOT/CODE/linux/patch_boot_on_img.sh" \
    "$ROOT/CODE/linux/qemu_verify_loop.sh" \
    "$ROOT/CODE/linux/guest/deshab-kde-session" \
    "$ROOT/ISO/run_qemu_kvm.sh" \
    "$ROOT/CODE/linux/grep_serial.sh"

echo "===== extra-rootfs probe (no drvfs loop mount) ====="
python3 - <<'PY'
from pathlib import Path
cands = [
    Path('/root/extra_rootfs_work/extra-rootfs.img'),
    Path('/home/deshab/linux-extra-rootfs.img'),
    Path('/home/deshab/extra-check.img'),
    Path('/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/linux-extra-rootfs.img'),
]
for p in cands:
    if p.exists():
        print(f'EXISTS {p} bytes={p.stat().st_size}')
    else:
        print(f'MISSING {p}')
PY

# Read-only debugfs on a NATIVE copy if present. Never mount the /mnt/d image.
NATIVE=""
for p in /root/extra_rootfs_work/extra-rootfs.img /home/deshab/linux-extra-rootfs.img; do
    if [ -f "$p" ]; then NATIVE="$p"; break; fi
done
if [ -n "$NATIVE" ]; then
    echo "[extra] debugfs $NATIVE"
    debugfs -R 'stat /usr/bin/startplasma-x11' "$NATIVE" 2>/dev/null | head -n 20 || true
    debugfs -R 'stat /usr/local/bin/deshab-kde-session' "$NATIVE" 2>/dev/null | head -n 8 || true
else
    echo "[extra] no native extra-rootfs image; skip debugfs"
fi

echo "===== rebuild initrd ====="
bash "$ROOT/CODE/linux/rebuild_initrd.sh"

echo "===== inject (rootfs only, no extra loop-mount) ====="
bash "$ROOT/CODE/linux/inject_boot_and_busybox.sh"
echo INJECT_OK

export QEMU_SEND_KDE=1
export QEMU_VERIFY_TIMEOUT=360
set +e
bash "$ROOT/CODE/linux/qemu_verify_loop.sh"
rc=$?
set -e
echo "VERIFY_RC=$rc"
echo "===== dump markers ====="
python3 - <<'PY'
from pathlib import Path
p = Path('/home/deshab/qemu_serial_loop1.log')
t = p.read_text('utf-8', errors='replace') if p.exists() else ''
needles = [
    'desktop ready', 'EPT misconfig', 'EPT reuse', 'EXEC_READY',
    '[deshab-kde]', 'execv failed', 'async spawn', 'async exec',
    'interpreter alive', 'entering KDE', 'startplasma', 'Plasma',
    'clear_page_erms', 'EXT4-fs error', 'allocate_slab',
    'KDE scanout', 'VERIFY_', 'serial 2',
]
lines = []
lines.append(f'bytes={len(t.encode("utf-8", "replace"))}')
for n in needles:
    lines.append(f'{n!r}: {t.find(n)}')
nv = [ln for ln in t.splitlines() if not ln.startswith('[VMEXIT]')
      and 'reason=' not in ln and 'qual=' not in ln]
lines.append('--- last 40 non-vmexit ---')
lines.extend(nv[-40:])
out = Path('/mnt/d/Code/DEAICUP/Deshab/.build_tmp/verify_dump7.txt')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('\n'.join(lines), encoding='utf-8')
print('wrote', out)
PY
exit 0
