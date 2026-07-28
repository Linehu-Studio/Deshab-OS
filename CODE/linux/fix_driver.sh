#!/bin/bash
# Fix UTSM driver entries in Linux source tree
SRC="${1:-/tmp/linux-6.6}"

# Fix Kconfig: remove broken line, add correct one
sed -i '/utsm/d' "$SRC/drivers/Kconfig"
echo 'source "drivers/utsm/Kconfig"' >> "$SRC/drivers/Kconfig"

# Fix Makefile: remove broken line, add correct one
sed -i '/utsm/d' "$SRC/drivers/Makefile"
echo 'obj-$(CONFIG_UTSM_HCALL) += utsm/' >> "$SRC/drivers/Makefile"

echo "Fixed UTSM driver entries"
tail -2 "$SRC/drivers/Kconfig"
echo "---"
tail -2 "$SRC/drivers/Makefile"
