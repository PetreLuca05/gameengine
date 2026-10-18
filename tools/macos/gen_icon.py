#!/usr/bin/env python3
"""Placeholder macOS app icon (tools/macos/AppIcon.png, 1024 px): a rounded dark tile with a red
crosshair and "DG" in Barlow SemiBold. tools/macos/make_bundle.sh turns it into Deadground.icns
(sips + iconutil). Replace the PNG with real art; the bundle script picks it up unchanged.

usage: gen_icon.py [out.png]   (needs Pillow)
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tools", "macos", "AppIcon.png")
S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# macOS icon grid: 824 px tile, 100 px margin, ~185 px corner radius
m, r = 100, 185
for i in range(S - 2 * m):   # vertical gradient
    t = i / (S - 2 * m)
    c = (int(34 - 14 * t), int(38 - 16 * t), int(36 - 14 * t), 255)
    d.line([(m, m + i), (S - m, m + i)], fill=c)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([m, m, S - m, S - m], r, fill=255)
img.putalpha(mask)
d = ImageDraw.Draw(img)
red = (196, 38, 32, 255)
cx, cy = S // 2, S // 2 - 30
d.ellipse([cx - 250, cy - 250, cx + 250, cy + 250], outline=red, width=34)
for a, b in (((cx, cy - 330), (cx, cy - 150)), ((cx, cy + 150), (cx, cy + 330)), ((cx - 330, cy), (cx - 150, cy)), ((cx + 150, cy), (cx + 330, cy))):
    d.line([a, b], fill=red, width=34)
font = ImageFont.truetype(os.path.join(ROOT, "assets", "ui", "fonts", "Barlow-SemiBold.ttf"), 250)
d.text((cx, cy), "DG", font=font, fill=(232, 228, 216, 255), anchor="mm")
os.makedirs(os.path.dirname(out), exist_ok=True)
img.save(out)
print("icon:", out)
