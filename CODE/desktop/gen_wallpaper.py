# -*- coding: utf-8 -*-
"""gen_wallpaper.py — 生成 Win11 风格抽象壁纸（无需外部图片素材）

以深海军蓝渐变为底，叠加多层低透明度旋转椭圆"花瓣"光晕
（模拟 Win11 Bloom），四角轻微暗角。输出 wallpaper.png。
"""
import math
from PIL import Image, ImageDraw, ImageFilter

W, H = 1000, 700

def main():
    # 底：垂直渐变 深蓝 #16264a → 暗蓝黑 #0a0e1c
    base = Image.new("RGB", (W, H))
    top = (22, 38, 74)
    bot = (9, 13, 27)
    px = base.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(top[0] * (1 - t) + bot[0] * t)
        g = int(top[1] * (1 - t) + bot[1] * t)
        b = int(top[2] * (1 - t) + bot[2] * t)
        for x in range(W):
            px[x, y] = (r, g, b)

    # Bloom 花瓣层：中心偏右上的多层旋转椭圆光晕
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx, cy = W * 0.58, H * 0.44
    petals = [
        # (长半轴, 短半轴, 角度deg, 颜色, alpha)
        (300, 130, 15,  (86, 130, 220), 46),
        (260, 100, -25, (110, 90, 200), 40),
        (230, 80, 50,   (60, 170, 220), 36),
        (200, 70, -60,  (140, 110, 230), 32),
        (170, 60, 85,   (70, 200, 190), 30),
        (150, 55, -100, (170, 130, 240), 26),
        (330, 150, 30,  (60, 100, 190), 22),
        (140, 50, 120,  (90, 150, 240), 24),
    ]
    for rx, ry, ang, col, alpha in petals:
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        # 椭圆边界框（绕中心旋转后取包围盒，再由 rotate 转正）
        bbox = [cx - rx, cy - ry, cx + rx, cy + ry]
        ld.ellipse(bbox, fill=col + (alpha,))
        layer = layer.rotate(ang, center=(cx, cy), resample=Image.BICUBIC)
        glow = Image.alpha_composite(glow, layer)

    # 大光晕柔化后叠加
    glow = glow.filter(ImageFilter.GaussianBlur(28))
    out = Image.alpha_composite(base.convert("RGBA"), glow)

    # 暗角：四角压暗（径向 alpha 蒙版）
    vign = Image.new("L", (W, H), 0)
    vd = ImageDraw.Draw(vign)
    steps = 24
    for i in range(steps):
        t = i / steps
        # 由外向内画黑色环带，alpha 递减
        shrink = int((1 - t) * 260)
        alpha = int(70 * (1 - t) ** 2)
        vd.ellipse([-shrink, -shrink, W + shrink, H + shrink],
                   outline=(alpha,), width=int(260 / steps) + 2)
    dark = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    dark.putalpha(vign)
    out = Image.alpha_composite(out, dark)

    out.convert("RGB").save("wallpaper.png")
    # 嵌入位图来源：PNG（人看）+ 原始 RGBA（裸机加载，无解码器）。
    # 注意：不能用 PIL 直接 save(".rgba")——该扩展被 SgiImagePlugin 认领，
    # 会写 512 字节 SGI 头。必须手写 tobytes()。
    with open("wallpaper.rgba", "wb") as f:
        f.write(out.convert("RGBA").tobytes())
    print("wallpaper.png/.rgba generated:", W, "x", H)


if __name__ == "__main__":
    main()
