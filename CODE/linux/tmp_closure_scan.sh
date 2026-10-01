#!/usr/bin/env bash
# Determine the recursive NEEDED closure of startplasma-x11 inside the
# extra-rootfs and intersect it with the corruption lists (TRUNCATED /
# NOT-ELF). Emits the repair package list.
set -uo pipefail
IMG="${1:?img}"
WORK=/home/deshab/repair_scan
rm -rf "$WORK"; mkdir -p "$WORK"

L=$(losetup --find --show --partscan "$IMG")
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M" || exit 1
R="$WORK/root"
mkdir -p "$R"
mount -o loop,ro "$M/boot/linux-extra-rootfs.img" "$R" || exit 1

# 1) NEEDED closure walk (pure userspace, no guest needed)
python3 - "$R" <<'PY' > "$WORK/closure.txt"
import os, struct, sys
root = sys.argv[1]
libdirs = [os.path.join(root, d) for d in
           ("usr/lib", "lib", "usr/lib64")]
def find(name):
    for d in libdirs:
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None
def needed(path):
    try:
        with open(path, "rb") as f:
            hdr = f.read(64)
        if hdr[:4] != b"\x7fELF":
            return []
        e_phoff = struct.unpack_from("<Q", hdr, 32)[0]
        e_phnum = struct.unpack_from("<H", hdr, 56)[0]
        with open(path, "rb") as f:
            f.seek(e_phoff)
            ph = f.read(56 * e_phnum)
        dynoff = dynsz = 0
        for i in range(e_phnum):
            off = i * 56
            p_type = struct.unpack_from("<I", ph, off)[0]
            if p_type == 2:  # PT_DYNAMIC
                dynoff = struct.unpack_from("<Q", ph, off + 8)[0]
                dynsz = struct.unpack_from("<Q", ph, off + 32)[0]
        if not dynoff:
            return []
        with open(path, "rb") as f:
            f.seek(dynoff)
            dyn = f.read(dynsz)
        strs = []
        for i in range(len(dyn) // 16):
            tag, val = struct.unpack_from("<QQ", dyn, i * 16)
            if tag == 1:  # DT_NEEDED
                strs.append(val)
            elif tag == 0:
                break
        # DT_STRTAB is vaddr; for these libs vaddr ~ file offset in first LOAD
        strtab = 0
        for i in range(len(dyn) // 16):
            tag, val = struct.unpack_from("<QQ", dyn, i * 16)
            if tag == 5:  # DT_STRTAB
                strtab = val
        out = []
        with open(path, "rb") as f:
            for v in strs:
                f.seek(strtab + v)
                b = f.read(128)
                out.append(b.split(b"\0")[0].decode("utf-8", "replace"))
        return out
    except (OSError, struct.error):
        return []

seen = set()
queue = [os.path.join(root, "usr/bin/startplasma-x11")]
while queue:
    p = queue.pop()
    if p in seen or not os.path.isfile(p):
        continue
    seen.add(p)
    for n in needed(p):
        q = find(n)
        print("%s -> %s" % (os.path.basename(p), n))
        if q:
            queue.append(q)
PY

sort -u "$WORK/closure.txt" > "$WORK/closure_sorted.txt"
# unique lib names in the closure
awk '{print $NF}' "$WORK/closure_sorted.txt" | sort -u > "$WORK/closure_libs.txt"

# 2) corruption lists
grep -E 'TRUNCATED|NOT-ELF' /home/deshab/elfscan_native.txt | awk '{print $NF}' | sort -u > "$WORK/bad_libs.txt"

# 3) intersection: bad libs that are actually on the Plasma path
comm -12 "$WORK/closure_libs.txt" "$WORK/bad_libs.txt" > "$WORK/bad_on_path.txt"

echo "=== closure libs: $(wc -l < "$WORK/closure_libs.txt") ==="
echo "=== bad libs total: $(wc -l < "$WORK/bad_libs.txt") ==="
echo "=== BAD ON PLASMA PATH: $(wc -l < "$WORK/bad_on_path.txt") ==="
cat "$WORK/bad_on_path.txt"
# closure edges referencing bad libs
echo "=== edges into bad libs ==="
grep -F -f "$WORK/bad_on_path.txt" "$WORK/closure_sorted.txt" | head -40

umount "$R"
umount "$M"; rmdir "$M"; losetup -d "$L"