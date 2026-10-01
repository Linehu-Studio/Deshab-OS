#!/usr/bin/env bash
# Assess the parallel session's state: QEMU running? new kernel markers? image ESP state?
echo '=== QEMU / ports ==='
pgrep -af 'qemu-system' | head -3 || echo 'no qemu'
ss -ltn 2>/dev/null | grep -E '45454|45455' || echo 'ports free'
echo '=== new kernel 992fa386 markers ==='
K=/mnt/d/Code/DEAICUP/Deshab/SYSTEM/boot/utsm.elf
strings -a "$K" | grep -c 'VGPU. FLUSH_N'
strings -a "$K" | grep -c 'selftest_irq\|tick 竞态' 2>/dev/null || true
echo '=== image ESP kernels now ==='
for img in /mnt/d/Code/DEAICUP/Deshab/ISO/deshab-dev.img /home/deshab/deshab-dev.img; do
    L=$(losetup --find --show --partscan "$img" 2>/dev/null) || { echo "$img: LOSETUP_FAIL"; continue; }
    sleep 0.3
    M=$(mktemp -d)
    if mount -o ro "${L}p1" "$M" 2>/dev/null; then
        echo "$(basename "$img"): $(md5sum "$M/boot/utsm.elf" 2>/dev/null | cut -d' ' -f1) FUCK_demo_ms=$(grep -a '^demo_ms' "$M/system/deshab64/FUCK" 2>/dev/null)"
        umount "$M"
    fi
    rmdir "$M" 2>/dev/null
    losetup -d "$L" 2>/dev/null
done
echo '=== parallel-edit check: sched.c mtime now ==='
stat -c '%y' /mnt/d/Code/DEAICUP/Deshab/CODE/UTSM/sched/sched.c | cut -d'.' -f1
ls -la --time-style='+%H:%M:%S' /mnt/d/Code/DEAICUP/Deshab/CODE/UTSM/obj 2>/dev/null | head -3