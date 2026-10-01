#!/usr/bin/env python3
"""Generate panic_logo_data.inc from ohMyLogo.png (root).

- Downscale logo to 384px wide (keep aspect), RGBA8888
- Sample the 4 corners (averaged) as the panic-screen background color
Usage: python gen_panic_logo.py <ohMyLogo.png> <panic_logo_data.inc>
"""
import sys
from PIL import Image

SRC = r"d:/Code/DEAICUP/Deshab/ohMyLogo.png"
DST = r"d:/Code/DEAICUP/Deshab/CODE/UTSM/kernel/panic_logo_data.inc"
TARGET_W = 384


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    dst = sys.argv[2] if len(sys.argv) > 2 else DST
    im = Image.open(src).convert("RGBA")
    w0, h0 = im.size
    scale = TARGET_W / w0
    w, h = TARGET_W, max(1, round(h0 * scale))
    im = im.resize((w, h), Image.LANCZOS)
    px = im.load()

    # background color: average of 4 corner pixels (5x5 patches)
    rs = gs = bs = n = 0
    for cx, cy in ((2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)):
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                r, g, b, _ = px[cx + dx, cy + dy]
                rs += r; gs += g; bs += b; n += 1
    r, g, b = rs // n, gs // n, bs // n
    bg = 0xFF000000 | (r << 16) | (g << 8) | b

    lines = []
    lines.append("/* Auto-generated from ohMyLogo.png — panic screen logo (RGBA8888) */")
    lines.append("/* DO NOT EDIT. Regenerate via: python gen_panic_logo.py */")
    lines.append("")
    lines.append(f"static const int g_panic_logo_w = {w};")
    lines.append(f"static const int g_panic_logo_h = {h};")
    lines.append(f"/* panic 界面背景色 = 图片四角平均色 RGB({r},{g},{b}) */")
    lines.append(f"static const unsigned int g_panic_logo_bg = 0x{bg:08X}u;")
    lines.append("static const unsigned char g_panic_logo_rgba[] = {")
    for y in range(h):
        row = []
        base = y * w
        for x in range(w):
            r, g, b, a = px[x, y]
            row.append(f"0x{r:02X},0x{g:02X},0x{b:02X},0x{a:02X}")
        lines.append("    " + ",".join(row) + ",")
    lines.append("};")
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"[gen_panic_logo] {w}x{h} bg=0x{bg:08X} -> {dst}")


if __name__ == "__main__":
    main()

