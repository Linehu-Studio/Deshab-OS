# -*- coding: utf-8 -*-
"""gen_textures.py — 桌面纹理资产转换（PNG/webP → 裸 RGBA 字节流）

输入（SYSTEM/system/deshab64/desktop/textures/）：
    back.webp        用户启动背景（889×500，缩放铺满 boot 屏）
    startMenuLogo.png 开始按钮/开始菜单 logo（216×210，带 alpha）

输出（同目录）：
    back.rgba         889×500×4
    startMenuLogo.rgba 216×210×4
    bootlogo.rgba     157×157×4（自 CODE/dsk/logo_data.c 的嵌入数据提取，
                      即 DSK 静态开机 Logo，desktop boot 屏复用）

注意：不能用 PIL 直接 save(".rgba")——扩展名被 SgiImagePlugin 认领（512B 头）。
"""
import os
import re
import sys

try:
    from PIL import Image
except ImportError:
    print("PIL required: pip install pillow", file=sys.stderr)
    sys.exit(1)

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
TEX_DIR = os.path.join(ROOT, "SYSTEM", "system", "deshab64", "desktop", "textures")
LOGO_C = os.path.join(ROOT, "CODE", "dsk", "logo_data.c")

# (源文件, 输出名, 期望宽, 期望高)
ASSETS = [
    ("back.webp",        "back.rgba",         889, 500),
    ("startMenuLogo.png", "startMenuLogo.rgba", 216, 210),
]


def save_raw(im, name, w, h):
    if im.size != (w, h):
        im = im.resize((w, h), Image.LANCZOS)
    data = im.convert("RGBA").tobytes()
    out = os.path.join(TEX_DIR, name)
    with open(out, "wb") as f:
        f.write(data)
    print("%s: %dx%d %d bytes" % (name, w, h, len(data)))


def extract_bootlogo():
    """从 DSK logo_data.c 提取 g_logo_rgba 字节数组 → bootlogo.rgba"""
    src = open(LOGO_C, "r", encoding="utf-8", errors="replace").read()
    m = re.search(r"g_logo_w\s*=\s*(\d+)", src)
    n = re.search(r"g_logo_h\s*=\s*(\d+)", src)
    if not m or not n:
        print("logo_data.c: no g_logo_w/h", file=sys.stderr)
        return False
    w, h = int(m.group(1)), int(n.group(1))
    body = re.search(r"g_logo_rgba\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not body:
        print("logo_data.c: no g_logo_rgba array", file=sys.stderr)
        return False
    vals = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", body.group(1))]
    expect = w * h * 4
    if len(vals) < expect:
        print("logo_data.c: %d bytes < expect %d" % (len(vals), expect), file=sys.stderr)
        return False
    out = os.path.join(TEX_DIR, "bootlogo.rgba")
    with open(out, "wb") as f:
        f.write(bytes(vals[:expect]))
    print("bootlogo.rgba: %dx%d %d bytes (from logo_data.c)" % (w, h, expect))
    return True


def main():
    os.makedirs(TEX_DIR, exist_ok=True)
    for src_name, out_name, w, h in ASSETS:
        src = os.path.join(TEX_DIR, src_name)
        if not os.path.exists(src):
            print("skip missing:", src_name)
            continue
        save_raw(Image.open(src), out_name, w, h)
    extract_bootlogo()


if __name__ == "__main__":
    main()
