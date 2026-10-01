#!/usr/bin/env bash
# Generate repair.txt: bind-mapping of good overlay copies over corrupt
# persist /usr/lib entities. Format per line: <chroot-src>|<chroot-dst>
set -uo pipefail
X=/home/deshab/xorg-libs
P=/home/deshab/plasma-libs
OUT=$X/repair.txt
: > "$OUT"
n=0; miss=0
while read -r name; do
    [ -z "$name" ] && continue
    src=""
    if [ -e "$X/$name" ]; then src="$X/$name"
    elif [ -e "$P/$name" ]; then src="$P/$name"
    else miss=$((miss+1)); echo "MISSING:$name" >> "$OUT.missing"; continue; fi
    # chroot-view source path (overlay dirs are bound at /usr/local/lib/*)
    case "$src" in
        $X/*) csrc="/usr/local/lib/xorg-libs/${src#$X/}" ;;
        $P/*) csrc="/usr/local/lib/plasma-libs/${src#$P/}" ;;
    esac
    printf '%s|%s\n' "$csrc" "/usr/lib/$name" >> "$OUT"
    n=$((n+1))
done < /home/deshab/notelf_list.txt
echo "mapped=$n missing=$miss -> $OUT"
wc -l "$OUT"