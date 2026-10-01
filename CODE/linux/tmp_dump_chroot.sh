#!/usr/bin/env bash
set -uo pipefail
python3 - <<'PY'
from pathlib import Path
p = Path('/home/deshab/qemu_serial_loop1.log')
t = p.read_bytes().decode('latin1', errors='replace')
t = t.replace('\r', '').replace('\x00', '')
keys = ('deshab-kde', 'utsm_bindmount', 'segfault', 'exec after', 'inside chroot',
        'starting Plasma', 'starting Xorg', 'ERROR', 'env:', 'bash:')
for line in t.splitlines():
    if any(k in line for k in keys):
        print(line[:220])
print('--- bytes', len(t))
PY
