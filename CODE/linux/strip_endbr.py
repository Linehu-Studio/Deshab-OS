#!/usr/bin/env python3
"""Replace ENDBR and the unsafe 0F 1F 40 00 NOP with 90 90 90 90.

Nested VMX leaks user CET: glibc ENDBR64 #GPs. Multi-byte NOP
`0F 1F 40 00` (`nop dword ptr [rax]`) can still #GP when RAX is 0.
Four 0x90 NOPs do not.
"""
from __future__ import annotations

import os
import sys

OLD = (
    b"\xf3\x0f\x1e\xfa",  # endbr64
    b"\xf3\x0f\x1e\xfb",  # endbr32
    b"\x0f\x1f\x40\x00",  # nop dword ptr [rax]
)
NEW = b"\x90\x90\x90\x90"


def is_elf(path: str) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def patch_file(path: str) -> int:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0
    if len(data) < 64 or data[:4] != b"\x7fELF":
        return 0
    n = 0
    out = data
    for old in OLD:
        c = out.count(old)
        if c:
            out = out.replace(old, NEW)
            n += c
    if n:
        with open(path, "wb") as f:
            f.write(out)
    return n


def walk(root: str) -> None:
    files = 0
    hits = 0
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames[:] = [d for d in dirnames if d not in ("proc", "sys", "dev", "run")]
        for name in filenames:
            path = os.path.join(dirpath, name)
            try:
                if os.path.islink(path) or not os.path.isfile(path):
                    continue
                if os.path.getsize(path) < 64:
                    continue
            except OSError:
                continue
            scanned += 1
            if scanned % 500 == 0:
                print(f"[endbr90] {root}: scanned={scanned} patched={files}", flush=True)
            if not is_elf(path):
                continue
            try:
                n = patch_file(path)
            except Exception as exc:
                print(f"[endbr90] skip {path}: {exc}", flush=True)
                continue
            if n:
                files += 1
                hits += n
    print(f"[endbr90] {root}: files={files} replacements={hits} scanned={scanned}", flush=True)


if __name__ == "__main__":
    for root in sys.argv[1:]:
        walk(root)
