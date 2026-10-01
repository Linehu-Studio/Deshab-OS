"""mkfont.py — Deshab Bitmap Font generator
Usage: python mkfont.py <ttf_path> <size> <output.dbf> [--charset charset.txt]

Renders GB2312 charset + ASCII (default) or a custom charset file to a packed
bitmap font file (.dbf).

Index format (see dbf.h):
  - Key is the Unicode BMP codepoint of the glyph (NOT the GB2312 byte pair).
  - Index entries are sorted ascending by codepoint — required by the
    binary-search reader dbf_lookup().

--charset: UTF-8 text file; every unique character in it becomes a glyph
(ASCII or CJK). Use to build small UI-subset fonts.
"""

import struct
import sys
from PIL import Image, ImageFont, ImageDraw

DBF_MAGIC = b"DBF\x10"


def gb2312_chars():
    """All GB2312 characters (level 1 + level 2) as a list of str."""
    chars = []
    # Level 1: 0xB0A1 - 0xD7F9 (pinyin ordered)
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    # Level 2: 0xD8A1 - 0xF7FE
    for hi in range(0xD8, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def charset_from_file(path):
    """Unique characters from a UTF-8 text file."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # 保留出现顺序去重；排序交给索引排序阶段
    return list(dict.fromkeys(text))


def render_font(ttf_path, size, output_path, charset=None, bpp=8):
    font = ImageFont.truetype(ttf_path, size)

    if charset is None:
        # 默认：ASCII 32-126 + 全 GB2312
        chars = [chr(c) for c in range(32, 127)] + gb2312_chars()
    else:
        chars = list(charset)
        # 去重
        chars = list(dict.fromkeys(chars))

    glyph_w = size
    glyph_h = size
    ascii_w = size * 3 // 4  # narrower for ASCII

    data_parts = []
    index_entries = []  # (unicode_cp, width, offset, size)

    for ch in chars:
        cp = ord(ch)
        if cp > 0xFFFF:
            continue  # dbf index key is u16 — BMP only
        w = ascii_w if cp < 128 else glyph_w

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

    # 关键：按 Unicode 码点升序排序（dbf_lookup 二分查找的前提），
    # 同码点去重（自定义字表可能重复包含同一字符）。
    seen = {}
    for cp, w, off, sz in index_entries:
        if cp not in seen:
            seen[cp] = (cp, w, off, sz)
    index_entries = sorted(seen.values(), key=lambda e: e[0])

    # Write file
    total_data = sum(len(d) for d in data_parts)
    # dbf.h 规范：data_offset 相对文件头（magic+header+index），写入时换算
    data_start = 14 + len(index_entries) * 12
    print(f"Glyphs: {len(index_entries)}, Data: {total_data} bytes")

    with open(output_path, "wb") as f:
        f.write(DBF_MAGIC)
        f.write(struct.pack("<I", len(index_entries)))
        f.write(struct.pack("<HHH", glyph_w, glyph_h, bpp))
        for cp, w, rel_off, sz in index_entries:
            f.write(struct.pack("<HHII", cp, w, data_start + rel_off, sz))
        for d in data_parts:
            f.write(d)


def main():
    argv = sys.argv[1:]
    charset_path = None
    if "--charset" in argv:
        i = argv.index("--charset")
        if i + 1 >= len(argv):
            print("--charset requires a file path")
            sys.exit(1)
        charset_path = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) != 3:
        print(f"Usage: {sys.argv[0]} <ttf_path> <size> <output.dbf> [--charset file]")
        sys.exit(1)
    ttf, size, out = argv[0], int(argv[1]), argv[2]
    charset = charset_from_file(charset_path) if charset_path else None
    if charset is not None:
        print(f"Custom charset: {len(charset)} unique chars from {charset_path}")
    render_font(ttf, size, out, charset=charset)


if __name__ == "__main__":
    main()
