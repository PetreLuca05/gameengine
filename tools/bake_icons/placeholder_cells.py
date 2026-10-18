#!/usr/bin/env python3
"""Placeholder item images (docs/INVENTORY.md "Cell images") for machines without the game's
renderer. The real images come from the game: tools/bake_icons/bake_icons.sh (square icons) and
tools/bake_icons/bake_cells.sh (grid-aspect cell images, bake_cells). This script only fills gaps:

  * assets/ui/cells/<id>.png missing: the item's baked 256 px icon, cropped to its silhouette and
    fitted (turned a quarter when that fills better) into the grid aspect, 64 px per cell; an item
    without an icon gets a drawn stand-in (its pickup shape and colour, shaded);
  * assets/ui/icons/{128,256}/<id>.png missing: the drawn stand-in, square.

Existing files are never overwritten (--force redoes the cells). Grid sizes: items.json "grid",
else the class defaults of src/gameplay/items.c (weapons in items.json carry theirs; attachment
items are 1x1, magnified optics 2x1).
Run: python3 tools/bake_icons/placeholder_cells.py [--force] [--list]"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CELL = 64


def items():
    out = []
    for it in json.load(open(os.path.join(ROOT, "data", "items.json"), encoding="utf-8"))["items"]:
        g = it.get("grid", [1, 1])
        out.append({"id": it["id"], "grid": g, "color": it.get("color", [0.7, 0.7, 0.7]), "shape": it.get("shape", "box")})
    adir = os.path.join(ROOT, "assets", "attachments")
    for f in sorted(os.listdir(adir)):
        if not f.endswith(".json"):
            continue
        a = json.load(open(os.path.join(adir, f), encoding="utf-8"))
        zoom = a.get("zoom", 1.0)
        g = [2, 1] if a.get("slot") == "optic" and isinstance(zoom, (int, float)) and zoom > 1.01 else [1, 1]
        out.append({"id": "att_" + a["id"], "grid": g, "color": [0.3, 0.3, 0.32], "shape": "box"})
    return out


def stand_in(w, h, color, shape):
    """The item's pickup shape in its colour: soft shadow, vertical shading, a rim."""
    s = 4
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = tuple(int(max(0.0, min(1.0, v)) * 255) for v in color[:3])
    m = int(min(w, h) * s * 0.14)
    box = [m, m, w * s - m, h * s - m]
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    off = int(min(w, h) * s * 0.04)
    sb = [box[0] + off, box[1] + off * 2, box[2] + off, box[3] + off * 2]
    if shape in ("sphere", "cylinder", "capsule"):
        sd.ellipse(sb, fill=(0, 0, 0, 110))
    else:
        sd.rounded_rectangle(sb, radius=m, fill=(0, 0, 0, 110))
    img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(off * 1.5)))
    body = Image.new("RGBA", img.size, (0, 0, 0, 0))
    bd = ImageDraw.Draw(body)
    for y in range(box[1], box[3]):
        t = (y - box[1]) / max(1, box[3] - box[1])
        k = 1.25 - 0.55 * t
        row = tuple(min(255, int(v * k + 18)) for v in c) + (255,)
        bd.line([(box[0], y), (box[2], y)], fill=row)
    mask = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(mask)
    if shape in ("sphere", "cylinder", "capsule"):
        md.ellipse(box, fill=255)
    else:
        md.rounded_rectangle(box, radius=m, fill=255)
    img.paste(body, (0, 0), mask)
    d = ImageDraw.Draw(img)
    rim = tuple(min(255, int(v * 0.5 + 120)) for v in c) + (200,)
    if shape in ("sphere", "cylinder", "capsule"):
        d.ellipse(box, outline=rim, width=max(2, s))
    else:
        d.rounded_rectangle(box, radius=m, outline=rim, width=max(2, s))
    return img.resize((w, h), Image.LANCZOS)


def fit_icon(icon, w, h):
    """The icon's silhouette (alpha bbox) fitted into w x h, turned a quarter if that fills better."""
    bbox = icon.getbbox()
    if bbox:
        icon = icon.crop(bbox)
    best = None
    for rot in (False, True):
        src = icon.rotate(90, expand=True) if rot else icon
        k = min(w * 0.9 / src.width, h * 0.9 / src.height)
        area = src.width * src.height * k * k
        if best is None or area > best[0] * 1.15:
            best = (area, src, k)
    _, src, k = best
    nw, nh = max(1, int(src.width * k)), max(1, int(src.height * k))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.alpha_composite(src.resize((nw, nh), Image.LANCZOS), ((w - nw) // 2, (h - nh) // 2))
    return out


def main():
    force = "--force" in sys.argv
    cells = os.path.join(ROOT, "assets", "ui", "cells")
    os.makedirs(cells, exist_ok=True)
    made = []
    for it in items():
        iid, (gw, gh) = it["id"], it["grid"]
        for size in (128, 256):
            p = os.path.join(ROOT, "assets", "ui", "icons", str(size), iid + ".png")
            if not os.path.exists(p):
                stand_in(size, size, it["color"], it["shape"]).save(p)
                made.append(os.path.relpath(p, ROOT))
        p = os.path.join(cells, iid + ".png")
        if os.path.exists(p) and not force:
            continue
        icon_p = os.path.join(ROOT, "assets", "ui", "icons", "256", iid + ".png")
        w, h = gw * CELL, gh * CELL
        img = fit_icon(Image.open(icon_p).convert("RGBA"), w, h) if os.path.exists(icon_p) else stand_in(w, h, it["color"], it["shape"])
        img.save(p)
        made.append(os.path.relpath(p, ROOT))
    if "--list" in sys.argv:
        for m in made:
            print(" ", m)
    print(f"placeholder_cells: {len(made)} file(s) written")


if __name__ == "__main__":
    main()
