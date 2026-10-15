"""contact_sheet.py <out.png> <cols> <img1> <img2> ...: downscaled grid with file names."""
import os
import sys

from PIL import Image, ImageDraw

out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
tw, th = 480, 270
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * tw, rows * (th + 16)), (20, 20, 20))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((tw, th), Image.LANCZOS)
    x, y = (i % cols) * tw, (i // cols) * (th + 16)
    sheet.paste(im, (x, y + 16))
    d.text((x + 4, y + 2), os.path.basename(f), fill=(255, 220, 140))
sheet.save(out)
