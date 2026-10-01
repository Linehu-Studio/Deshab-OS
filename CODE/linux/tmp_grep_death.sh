set -uo pipefail
LOG=/home/deshab/qemu_serial_loop1.log
echo "===== after startplasma ====="
grep -nE 'fatal signal|Plasma failed|SET_SCANOUT|plasmashell|startplasma|invalid ELF|xorg:' "$LOG" | tail -n 30
echo DONE