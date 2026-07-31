#!/usr/bin/env python3
"""Convert Logo.png to logo_data.c (RGBA8888 C array) for DSK kernel embedding.

Usage: python png_to_rgba.py <input.png> <output.c>

Output format:
  static const unsigned char g_logo_rgba[] = { ... };
  static const int g_logo_w = <width>;
  static const int g_logo_h = <height>;
"""

import sys
from PIL import Image

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.c>", file=sys.stderr)
        sys.exit(1)

    src_path = sys.argv[1]
    dst_path = sys.argv[2]

    img = Image.open(src_path).convert("RGBA")
    w, h = img.size
    raw = img.tobytes()  # RGBA byte order

    # Write C source file
    with open(dst_path, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated from Logo.png — RGBA8888 pixel data */\n")
        f.write("/* DO NOT EDIT. Regenerate via: python png_to_rgba.py Logo.png logo_data.c */\n\n")
        f.write(f"static const int g_logo_w = {w};\n")
        f.write(f"static const int g_logo_h = {h};\n")
        f.write(f"static const unsigned char g_logo_rgba[] = {{\n")

        bytes_per_row = w * 4
        for y in range(h):
            row_start = y * bytes_per_row
            row_data = raw[row_start:row_start + bytes_per_row]
            hex_vals = [f"0x{b:02x}" for b in row_data]
            f.write("    " + ", ".join(hex_vals) + ",\n")

        f.write("};\n")

    total = w * h * 4
    print(f"[png_to_rgba] {src_path} -> {dst_path}: {w}x{h}, {total} bytes")

if __name__ == "__main__":
    main()
