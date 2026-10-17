#!/usr/bin/env python3
"""Map thumbnails for the main menu (docs/MENU_LOBBY.md "Adding a map"): 384 x 216 PNGs in
assets/maps/thumbs/.

    python tools/gen_map_thumbs.py [--preview <id>=<png> ...]

* Brush levels (data/levels/<level>.json with "brushes"): a top-down plan of the brushes.
* Open-world levels: pass a tools/mapgen_preview render with --preview <id>=<png>
  (mapgen_preview data/levels/zombieland.json zl.png 8).
* Procedural terrain ("streaming"): hill-shaded value noise.
Every thumbnail gets the same grade (dark, slightly desaturated, vignette) so the list reads as one
set over the menu's dark panels. Needs Pillow.
"""
import json
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "maps", "thumbs")
W, H = 384, 216


def grade(img):
    img = img.convert("RGB").resize((W, H), Image.LANCZOS)
    img = ImageEnhance.Color(img).enhance(0.8)
    img = ImageEnhance.Brightness(img).enhance(0.82)
    # Vignette: darken towards the corners.
    vig = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(vig)
    for i in range(24):
        t = i / 23.0
        d.rectangle([int(t * W * 0.18), int(t * H * 0.18), W - int(t * W * 0.18), H - int(t * H * 0.18)], fill=int(255 * (0.35 + 0.65 * t)))
    vig = vig.filter(ImageFilter.GaussianBlur(18))
    black = Image.new("RGB", (W, H), (8, 9, 10))
    return Image.composite(img, black, vig)


def crop_fill(img):
    sw, sh = img.size
    scale = max(W / sw, H / sh)
    img = img.resize((max(W, int(sw * scale)), max(H, int(sh * scale))), Image.LANCZOS)
    x = (img.size[0] - W) // 2
    y = (img.size[1] - H) // 2
    return img.crop((x, y, x + W, y + H))


def brush_plan(level_path, focus=None):
    d = json.load(open(level_path))
    brushes = [b for b in d.get("brushes", []) if b.get("type", "box") == "box" and "pos" in b and "size" in b]
    xs = [b["pos"][0] for b in brushes]
    zs = [b["pos"][2] for b in brushes]
    if focus:
        cx, cz, half = focus
    else:
        cx, cz = (min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2
        half = max(max(xs) - min(xs), (max(zs) - min(zs)) * W / H) / 2 + 6
    k = 2  # supersample
    img = Image.new("RGB", (W * k, H * k), (52, 54, 56))
    dr = ImageDraw.Draw(img)
    ppm = W * k / (2 * half)
    # Floor grid (every 4 m).
    for gx in range(int(cx - half) // 4 * 4, int(cx + half) + 4, 4):
        x = (gx - cx) * ppm + W * k / 2
        dr.line([(x, 0), (x, H * k)], fill=(60, 62, 64), width=1)
    for gz in range(int(cz - half) // 4 * 4, int(cz + half) + 4, 4):
        y = (gz - cz) * ppm + H * k / 2
        dr.line([(0, y), (W * k, y)], fill=(60, 62, 64), width=1)
    for b in sorted(brushes, key=lambda b: b["pos"][1] + b["size"][1]):
        px, pz = b["pos"][0], b["pos"][2]
        sx, sz = b["size"][0] / 2, b["size"][2] / 2
        yaw = math.radians(b.get("yaw", 0))
        c, s = math.cos(yaw), math.sin(yaw)
        pts = []
        for ox, oz in ((-sx, -sz), (sx, -sz), (sx, sz), (-sx, sz)):
            wx = px + ox * c + oz * s
            wz = pz - ox * s + oz * c
            pts.append(((wx - cx) * ppm + W * k / 2, (wz - cz) * ppm + H * k / 2))
        col = b.get("color", [0.6, 0.6, 0.6])
        hgt = b["pos"][1] + b["size"][1]
        shade = 0.75 + min(hgt, 6.0) / 6.0 * 0.35   # taller = lighter (sun from above)
        rgb = tuple(int(max(0, min(255, v * 255 * shade))) for v in col[:3])
        dr.polygon(pts, fill=rgb, outline=(20, 20, 22))
    return img.resize((W, H), Image.LANCZOS)


def value_noise(seed=7):
    rnd = random.Random(seed)
    g = 24
    grid = [[rnd.random() for _ in range(g + 1)] for _ in range(g + 1)]

    def smooth(t):
        return t * t * (3 - 2 * t)

    def at(x, y):
        x0, y0 = int(x), int(y)
        fx, fy = smooth(x - x0), smooth(y - y0)
        a = grid[y0 % g][x0 % g] * (1 - fx) + grid[y0 % g][(x0 + 1) % g] * fx
        b = grid[(y0 + 1) % g][x0 % g] * (1 - fx) + grid[(y0 + 1) % g][(x0 + 1) % g] * fx
        return a * (1 - fy) + b * fy

    img = Image.new("RGB", (W, H))
    px = img.load()
    hts = [[0.0] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            h = 0.0
            amp, freq = 1.0, 1.0 / 48
            for _ in range(4):
                h += at(x * freq, y * freq) * amp
                amp *= 0.5
                freq *= 2
            hts[y][x] = h
    for y in range(H):
        for x in range(W):
            h = hts[y][x]
            dx = hts[y][min(x + 1, W - 1)] - hts[y][max(x - 1, 0)]
            dy = hts[min(y + 1, H - 1)][x] - hts[max(y - 1, 0)][x]
            light = max(0.25, min(1.2, 0.8 - (dx + dy) * 6))
            base = (70, 98, 58) if h < 1.05 else ((104, 96, 80) if h < 1.3 else (150, 150, 146))
            px[x, y] = tuple(int(min(255, v * light)) for v in base)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    previews = {}
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--preview" and i + 1 < len(args):
            k, v = args[i + 1].split("=", 1)
            previews[k] = v
    jobs = {
        "devroom": lambda: brush_plan(os.path.join(ROOT, "data", "levels", "devroom.json"), focus=(-30, 10, 85)),
        "terrain": value_noise,
    }
    for k, v in previews.items():
        jobs[k] = (lambda p: (lambda: crop_fill(Image.open(p).convert("RGB"))))(v)
    for name, fn in jobs.items():
        try:
            img = grade(fn())
        except Exception as e:  # a missing level / preview: skip, keep the old thumbnail
            print(f"{name}: skipped ({e})")
            continue
        path = os.path.join(OUT, name + ".png")
        img.save(path, optimize=True)
        print(f"{name}: {os.path.relpath(path, ROOT)}")


if __name__ == "__main__":
    main()
