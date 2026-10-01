#!/usr/bin/env bash
# Full ELF health scan of the extra-rootfs /usr/lib (read-only loop mount).
# Detects: non-ELF files, smashed headers (e_entry=0), truncated files
# (any PT_LOAD p_offset+p_filesz > file size -> ld.so SIGBUS on mmap tail).
set -uo pipefail
IMG="${1:?usage: tmp_elf_scan.sh IMG}"
L=$(losetup --find --show --partscan "$IMG" 2>/dev/null) || { echo LOSETUP_FAIL; exit 1; }
sleep 0.3
M=$(mktemp -d)
mount -o ro "${L}p1" "$M" || { echo ESP_MOUNT_FAIL; exit 1; }
R=$(mktemp -d)
mount -o loop,ro "$M/boot/linux-extra-rootfs.img" "$R" || { echo EXTRA_MOUNT_FAIL; exit 1; }

python3 - "$R" <<'PY'
import os, struct, sys
root = sys.argv[1]
dirs = [os.path.join(root, "usr", "lib"), os.path.join(root, "lib")]
bad = {}
checked = 0
for d in dirs:
    if not os.path.isdir(d):
        continue
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        if not (name.endswith(".so") or ".so." in name):
            continue
        checked += 1
        try:
            sz = os.path.getsize(p)
            with open(p, "rb") as f:
                hdr = f.read(64)
            if len(hdr) < 64 or hdr[:4] != b"\x7fELF":
                bad[p] = "NOT-ELF(%d)" % sz
                continue
            if hdr[4] != 2 or hdr[5] != 1:  # 64-bit LE
                bad[p] = "BAD-CLASS"
                continue
            # Explicit offsets: e_type@16 e_entry@24 e_phoff@32
            # e_phentsize@54 e_phnum@56 (ELF64 little-endian)
            e_type = struct.unpack_from("<H", hdr, 16)[0]
            e_entry = struct.unpack_from("<Q", hdr, 24)[0]
            e_phoff = struct.unpack_from("<Q", hdr, 32)[0]
            e_phentsize = struct.unpack_from("<H", hdr, 54)[0]
            e_phnum = struct.unpack_from("<H", hdr, 56)[0]
            if e_type not in (2, 3):  # ET_EXEC / ET_DYN
                bad[p] = "BAD-TYPE(%d)" % e_type
                continue
            if e_entry == 0 and e_type == 3:  # ET_DYN with null entry
                bad[p] = "ENTRY0"
            if e_phentsize < 56 or e_phnum == 0 or e_phnum > 128:
                bad[p] = "PHDR-GARBAGE(phentsize=%d phnum=%d)" % (e_phentsize, e_phnum)
                continue
            end = 0
            with open(p, "rb") as f:
                f.seek(e_phoff)
                ph = f.read(e_phentsize * e_phnum)
            if len(ph) < e_phentsize * e_phnum:
                bad[p] = "TRUNCATED(phdr table file=%d)" % sz
                continue
            for i in range(e_phnum):
                off = i * e_phentsize
                p_type, p_flags = struct.unpack_from("<II", ph, off)
                p_offset = struct.unpack_from("<Q", ph, off + 8)[0]
                # ELF64 Phdr: p_offset@8 p_vaddr@16 p_paddr@24
                # p_filesz@32 p_memsz@40 p_align@48
                p_filesz = struct.unpack_from("<Q", ph, off + 32)[0]
                if p_type == 1:  # PT_LOAD
                    end = max(end, p_offset + p_filesz)
            if end > sz:
                bad.setdefault(p, "TRUNCATED(phdr=%d file=%d short=%d)" % (end, sz, end - sz))
        except OSError as e:
            bad[p] = "IOERR:%s" % e
print("checked=%d bad=%d" % (checked, len(bad)))
for p, why in sorted(bad.items()):
    print("BAD %-12s %s" % (why, os.path.basename(p)))
PY

umount "$R"; rmdir "$R"
umount "$M"; rmdir "$M"
losetup -d "$L"