#!/usr/bin/env bash
# Strip CR from serial capture logs so they can be read as plain text.
set -u
cd "$(dirname "$0")/../../.build_tmp" || exit 1
for f in "$@"; do
    [ -f "$f" ] || { echo "missing: $f"; continue; }
    out="${f%.txt}_clean.txt"
    # drop CR plus any control/graphics bytes the read tool would treat as binary
    tr -cd '\11\12\40-\176' < "$f" > "$out"
    echo "ok $f -> $out ($(wc -l < "$out") lines)"
done