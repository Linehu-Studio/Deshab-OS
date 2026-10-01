"""mkfont_check.py — .dbf 字库校验工具

Usage: python mkfont_check.py <font.dbf> [字符...]

校验项：
  1. magic "DBF\x10"、bpp、字形尺寸
  2. 索引按 codepoint 升序（dbf_lookup 二分查找前提）
  3. 抽样码点查找（默认：中文测试开始菜单任务管理器设置搜索通知中心
     关机重启睡眠亮暗主题浅深任务栏窗口图标网络音量电源时间日期）
  4. 无字符参数时把「中」渲染为 ASCII art 供肉眼检查
"""

import struct
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DBF_MAGIC = b"DBF\x10"
DEFAULT_SAMPLE = "中文测试开始菜单任务管理器设置搜索通知中心关机重启睡眠亮暗主题浅深任务栏窗口图标网络音量电源时间日期"


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != DBF_MAGIC:
        raise SystemExit(f"BAD MAGIC: {data[:4]!r}")
    count = struct.unpack_from("<I", data, 4)[0]
    w, h, bpp = struct.unpack_from("<HHH", data, 8)
    idx = []
    for i in range(count):
        cp, gw, off, sz = struct.unpack_from("<HHII", data, 14 + i * 12)
        idx.append((cp, gw, off, sz))
    return data, count, w, h, bpp, idx


def lookup(idx, cp):
    lo, hi = 0, len(idx) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if idx[mid][0] == cp:
            return idx[mid]
        if idx[mid][0] < cp:
            lo = mid + 1
        else:
            hi = mid - 1
    return None


def ascii_art(data, entry, h):
    _, w, off, _sz = entry
    print(f"  +{'-' * w}+")
    for y in range(h):
        row = " "
        for x in range(w):
            v = data[off + y * w + x]
            row += "█" if v > 200 else ("▓" if v > 120 else ("░" if v > 50 else " "))
        print("  |" + row + "|")
    print(f"  +{'-' * w}+")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <font.dbf> [chars]")
        sys.exit(1)
    path = sys.argv[1]
    data, count, w, h, bpp, idx = load(path)
    print(f"{path}: count={count} glyph={w}x{h} bpp={bpp}")

    # 校验升序
    bad = sum(1 for a, b in zip(idx, idx[1:]) if a[0] >= b[0])
    if bad:
        raise SystemExit(f"INDEX NOT SORTED: {bad} violations")
    print("index sorted: OK")

    sample = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_SAMPLE
    missing = [c for c in sample if lookup(idx, ord(c)) is None]
    if missing:
        print(f"MISSING GLYPHS: {''.join(missing)}")
        sys.exit(1)
    print(f"all {len(sample)} sample glyphs found: OK")

    # 肉眼检查首字符
    first = sample[0]
    ascii_art(data, lookup(idx, ord(first)), h)


if __name__ == "__main__":
    main()
