"""render_embedded.py — render FirstInit Chinese strings at varied sizes"""
from PIL import Image, ImageFont, ImageDraw

ttf = "SYSTEM/system/font/simhei.ttf"

texts = [
    ("welcome", "欢迎使用 Deshab", 36),
    ("setup",   "接下来让我们来引导你设置你的系统", 36),
    ("title",   "创建你的 Deshab 账户", 24),
    ("computer", "计算机名", 24),
    ("username", "用户名", 24),
    ("password", "密码", 24),
    ("hint", "按 Enter 继续，Backspace 删除", 16),  # 20 -> 16
    ("prefs_title", "设置系统偏好", 24),
    ("prefs_language", "语言", 22),
    ("prefs_region", "地区", 22),
    ("prefs_timezone", "时区", 22),
    ("prefs_keyboard", "键盘布局", 22),
    ("prefs_theme", "主题", 22),
    ("prefs_hint", "Enter 切换选项，方向键切换项目，鼠标点击选择", 16),
    ("network_title", "网络设置", 24),
    ("net_mode", "网络模式", 22),
    ("net_device", "网络设备", 22),
    ("net_ip", "地址获取", 22),
    ("net_dns", "DNS", 22),
    ("net_hint", "Enter 切换选项，方向键切换项目，鼠标点击选择", 16),
    ("privacy_title", "隐私与诊断", 24),
    ("done", "设置完成，请重启系统", 24),       # final status
]

lines = []
lines.append("/* Auto-generated */")
lines.append("")

for name, text, size in texts:
    font = ImageFont.truetype(ttf, size)
    bbox = font.getbbox(text)
    w = bbox[2]
    h = size + 6
    img = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, 3), text, font=font, fill=255)
    pixels = list(img.tobytes())

    lines.append(f"static const int g_txt_{name}_w = {w};")
    lines.append(f"static const int g_txt_{name}_h = {h};")
    lines.append(f"static const unsigned char g_txt_{name}[{w}*{h}] = {{")
    for y in range(h):
        row = ", ".join(str(pixels[y*w + x]) for x in range(w))
        lines.append(f"  {row},")
    lines.append("};")
    lines.append("")

with open("CODE/firstInit/text_bitmaps.c", "w", encoding="ascii") as f:
    f.write("\n".join(lines))

print("Generated CODE/firstInit/text_bitmaps.c")
