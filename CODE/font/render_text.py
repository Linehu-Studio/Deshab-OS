"""render_text.py — pre-render specific Chinese strings to C arrays"""
from PIL import Image, ImageFont, ImageDraw

def render(ttf, texts, size, var_prefix):
    font = ImageFont.truetype(ttf, size)
    for name, text in texts:
        w = font.getbbox(text)[2]
        h = size + 4
        img = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((0, 2), text, font=font, fill=255)

        pixels = list(img.tobytes())
        print(f"/* {text} — {w}x{h} */")
        print(f"static const int {var_prefix}_{name}_w = {w};")
        print(f"static const int {var_prefix}_{name}_h = {h};")
        print(f"static const unsigned char {var_prefix}_{name}_data[{w}*{h}] = {{")
        for y in range(h):
            row = ", ".join(str(pixels[y*w + x]) for x in range(w))
            print(f"  {row},")
        print("};")
        print()

texts = [
    ("welcome", "欢迎使用 Deshab"),
    ("setup",   "接下来让我们来引导你设置你的系统"),
]
# Generate for multiple sizes
for sz in [24]:
    print(f"// ===== size={sz}px =====")
    render("SYSTEM/system/font/simhei.ttf", texts, sz, f"txt{sz}")
