#!/usr/bin/env bash
# Extract unique NOT-ELF lib names from the scan output.
set -uo pipefail
F=/home/deshab/elfscan_native.txt
grep NOT-ELF "$F" | awk '{print $NF}' | sort -u > /home/deshab/notelf_list.txt
wc -l /home/deshab/notelf_list.txt
cat /home/deshab/notelf_list.txt | tr '\n' ' '
echo