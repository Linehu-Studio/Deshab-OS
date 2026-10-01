#!/usr/bin/env python3
"""Convert a PNG to a prefixed RGBA8888 C array for DSK kernel embedding.

Usage: python png_to_rgba_prefixed.py <input.png> <output.c> <prefix>

Output symbols:
  static const int <prefix>_w / <prefix>_h / <prefix>_rgba[]
"""

import sys
from PIL import Image

# Cap so embedded arrays stay small relative to typical 1024x768 GOP mode
MAX_W, MAX_H = 1000, 700

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.png> <output.c> <prefix>", file=sys.stderr)
        sys.exit(1)

    src_path, dst_path, prefix = sys.argv[1], sys.argv[2], sys.argv[3]

    img = Image.open(src_path).convert("RGBA")
    if img.width > MAX_W or img.height > MAX_H:
        scale = min(MAX_W / img.width, MAX_H / img.height)
        img = img.resize((max(1, round(img.width * scale)),
                          max(1, round(img.height * scale))),
                         Image.LANCZOS)
    w, h = img.size
    raw = img.tobytes()  # RGBA byte order

    with open(dst_path, "w", encoding="utf-8") as f:
        f.write(f"/* Auto-generated from {src_path} - RGBA8888 pixel data */\n")
        f.write(f"/* DO NOT EDIT. Regenerate via: python png_to_rgba_prefixed.py {src_path} {dst_path} {prefix} */\n\n")
        f.write(f"static const int {prefix}_w = {w};\n")
        f.write(f"static const int {prefix}_h = {h};\n")
        f.write(f"static const unsigned char {prefix}_rgba[] = {{\n")

        bytes_per_row = w * 4
        for y in range(h):
            row_start = y * bytes_per_row
            row_data = raw[row_start:row_start + bytes_per_row]
            hex_vals = [f"0x{b:02x}" for b in row_data]
            f.write("    " + ", ".join(hex_vals) + ",\n")

        f.write("};\n")

    total = w * h * 4
    print(f"[png_to_rgba_prefixed] {src_path} -> {dst_path}: {w}x{h}, {total} bytes")

if __name__ == "__main__":
    main()