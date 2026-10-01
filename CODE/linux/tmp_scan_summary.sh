#!/usr/bin/env bash
# Summarize the real corruption classes from the ELF scan output.
set -uo pipefail
F=/tmp/elfscan_native.txt
echo "=== TRUNCATED (SIGBUS-causing) ==="
grep TRUNCATED "$F" | awk '{print $NF}' | sort -u
echo "=== NOT-ELF unique count ==="
grep NOT-ELF "$F" | awk '{print $NF}' | sort -u | wc -l
echo "=== NOT-ELF list ==="
grep NOT-ELF "$F" | awk '{print $NF}' | sort -u | tr '\n' ' '
echo