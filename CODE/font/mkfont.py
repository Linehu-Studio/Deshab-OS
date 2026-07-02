"""mkfont.py — Deshab Bitmap Font generator
Usage: python mkfont.py <ttf_path> <size> <output.dbf>
Renders GB2312 charset + ASCII to a packed bitmap font file (.dbf)
"""

import struct
import sys
from PIL import Image, ImageFont, ImageDraw

DBF_MAGIC = b"DBF\x10"

# GB2312 codepoints for simplified Chinese (3755 level-1 + 3008 level-2)
def gb2312_codepoints():
    cps = []
    # ASCII 32-126
    for c in range(32, 127):
        cps.append(c)
    # GB2312 level 1: 0xB0A1 - 0xB0FE, ... , 0xD7F9
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            cps.append((hi << 8) | lo)
    # GB2312 level 2: skip duplicates, 0xD8A1 - 0xF7FE
    for hi in range(0xD8, 0xF8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            cps.append((hi << 8) | lo)
    return cps


def render_font(ttf_path, size, output_path, bpp=8):
    font = ImageFont.truetype(ttf_path, size)
    cps = gb2312_codepoints()

    # Build index and data
    glyph_w = size
    glyph_h = size
    data_parts = []
    index_entries = []

    ascii_w = size * 3 // 4  # narrower for ASCII

    for cp in cps:
        if cp < 128:
            # ASCII — use narrower glyph
            ch = chr(cp)
            w = ascii_w
        else:
            # GB2312 byte pair → Unicode
            hi = (cp >> 8) & 0xFF
            lo = cp & 0xFF
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except:
                continue
            w = glyph_w

        img = Image.new("L", (w, glyph_h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((0, 0), ch, font=font, fill=255)
        img = img.resize((w, glyph_h), Image.LANCZOS)

        raw = img.tobytes()
        if bpp == 1:
            # pack to 1bpp
            packed = bytearray()
            for y in range(glyph_h):
                row = 0
                for x in range(w):
                    if raw[y * w + x] >= 128:
                        row |= (1 << (7 - (x & 7)))
                    if (x & 7) == 7:
                        packed.append(row)
                        row = 0
                if w & 7:
                    packed.append(row)
            glyph_data = bytes(packed)
        else:
            glyph_data = raw

        offset = sum(len(d) for d in data_parts)
        data_parts.append(glyph_data)
        index_entries.append((cp, w, offset, len(glyph_data)))

    # Write file
    total_data = sum(len(d) for d in data_parts)
    print(f"Glyphs: {len(index_entries)}, Data: {total_data} bytes")

    with open(output_path, "wb") as f:
        f.write(DBF_MAGIC)
        f.write(struct.pack("<I", len(index_entries)))
        f.write(struct.pack("<HHH", glyph_w, glyph_h, bpp))
        for cp, w, off, sz in index_entries:
            f.write(struct.pack("<HHII", cp, w, off, sz))
        for d in data_parts:
            f.write(d)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <ttf_path> <size> <output.dbf>")
        sys.exit(1)
    ttf = sys.argv[1]
    sz = int(sys.argv[2])
    out = sys.argv[3]
    render_font(ttf, sz, out)
