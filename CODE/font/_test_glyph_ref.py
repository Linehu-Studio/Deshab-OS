# -*- coding: utf-8 -*-
"""_test_glyph_ref.py — dbf 字形与 TTF 直接渲染逐字节比对（码点映射 + 数据无损验证）"""
import struct
from PIL import Image, ImageFont, ImageDraw

def load(path):
    data = open(path, "rb").read()
    count = struct.unpack_from("<I", data, 4)[0]
    w, h, bpp = struct.unpack_from("<HHH", data, 8)
    idx = [struct.unpack_from("<HHII", data, 14 + i * 12) for i in range(count)]
    return data, w, h, idx

def lookup(idx, cp):
    lo, hi = 0, len(idx) - 1
    while lo <= hi:
        m = (lo + hi) // 2
        if idx[m][0] == cp:
            return idx[m]
        if idx[m][0] < cp:
            lo = m + 1
        else:
            hi = m - 1
    return None

SAMPLES = "中文测试开始菜单任务管理器设置搜索通知中心关机重启睡眠亮暗主题浅深任务栏窗口图标网络音量电源时间日期锁屏用户名睡眠退出Deshab 0123"

for name, size in [("simhei_16.dbf", 16), ("simhei_24.dbf", 24)]:
    data, w, h, idx = load(name)
    font = ImageFont.truetype("simhei.ttf", size)
    ascii_w = size * 3 // 4
    ok = bad = 0
    diffs = []
    for ch in SAMPLES:
        cp = ord(ch)
        e = lookup(idx, cp)
        if not e:
            diffs.append((ch, "MISSING"))
            bad += 1
            continue
        _, gw, off, sz = e
        dbf_glyph = data[off:off + sz]
        ref_w = ascii_w if cp < 128 else size
        img = Image.new("L", (ref_w, h), 0)
        ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=255)
        img = img.resize((ref_w, h), Image.LANCZOS)
        ref = img.tobytes()
        if dbf_glyph == ref and gw == ref_w:
            ok += 1
        else:
            bad += 1
            nd = sum(1 for a, b in zip(dbf_glyph, ref) if a != b)
            diffs.append((hex(cp), "bytes-diff", nd, "/", len(ref)))
    print(name, "exact-match:", ok, "mismatch:", bad)
    for d in diffs[:6]:
        print("  diff:", d)
