"""Ground reference sampler (docs/GROUND_REFERENCE.md): colour statistics of the reference regions and the
blade-by-blade reference sheet.

    py tools/ground_ref.py                 # stats.json + blade_sheet.png in docs/reference/ground/
    py tools/ground_ref.py --print         # also print the colour table (markdown)

Inputs: docs/reference/ground/regions.json (images, regions, blade crops). Masks (sRGB HSV, h in degrees):
  green  h 55..160, s > 0.25, v > 0.22     (living blades / leaves)
  dry    h 25..70,  0.10 < s < 0.60, v > 0.42   (straw, dry tips, wheat)
  dark   v < 0.45                          (ground between blades)
  plant  green | dry                       (blade crops: the gradient profile)
  all    every pixel of the box
Outputs: per region the pixel fraction, mean sRGB (hex), mean linear RGB, HSV mean (circular hue) and std, the
5 / 50 / 95 % value; per blade crop root / mid / tip means (bottom / middle / top third of the plant pixels)
and the blade sheet (crops scaled to a common height, the plant mask beside each).
"""
import argparse
import colorsys
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
REF = os.path.join(ROOT, "docs", "reference", "ground")
SHEET_H = 256


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def hsv(rgb):
    """rgb: (N, 3) in 0..1 -> (N, 3) h in degrees, s, v."""
    mx = rgb.max(1)
    mn = rgb.min(1)
    d = mx - mn
    s = np.where(mx > 0, d / np.maximum(mx, 1e-6), 0)
    r, g, b = rgb[:, 0], rgb[:, 1], rgb[:, 2]
    h = np.zeros_like(mx)
    dd = np.maximum(d, 1e-6)
    h = np.where(mx == r, ((g - b) / dd) % 6, h)
    h = np.where(mx == g, (b - r) / dd + 2, h)
    h = np.where(mx == b, (r - g) / dd + 4, h)
    h = np.where(d > 0, h * 60.0, 0)
    return np.stack([h, s, mx], 1)


def mask_of(rgb, kind):
    h, s, v = hsv(rgb).T
    green = (h >= 55) & (h <= 160) & (s > 0.25) & (v > 0.22)
    dry = (h >= 25) & (h <= 70) & (s > 0.10) & (s < 0.60) & (v > 0.42)
    if kind == "green":
        return green
    if kind == "dry":
        return dry & ~green
    if kind == "plant":
        return green | dry
    if kind == "dark":
        return v < 0.45
    return np.ones(len(rgb), bool)


def stats(rgb):
    if len(rgb) == 0:
        return None
    lin = srgb_to_lin(rgb)
    hs = hsv(rgb)
    ang = np.radians(hs[:, 0])
    hm = math.degrees(math.atan2(np.sin(ang).mean(), np.cos(ang).mean())) % 360
    mean = rgb.mean(0)
    return {
        "hex": "#%02x%02x%02x" % tuple(int(round(x * 255)) for x in mean),
        "srgb": [round(float(x), 4) for x in mean],
        "linear": [round(float(x), 4) for x in lin.mean(0)],
        "hsv_mean": [round(hm, 1), round(float(hs[:, 1].mean()), 3), round(float(hs[:, 2].mean()), 3)],
        "hsv_std": [round(float(np.degrees(np.sqrt(-2 * np.log(max(1e-9, math.hypot(np.sin(ang).mean(), np.cos(ang).mean())))))), 1),
                    round(float(hs[:, 1].std()), 3), round(float(hs[:, 2].std()), 3)],
        "v_p5_p50_p95": [round(float(x), 3) for x in np.percentile(hs[:, 2], [5, 50, 95])],
    }


def load(cfg, key):
    im = Image.open(os.path.join(REF, cfg["images"][key]["file"])).convert("RGB")
    return np.asarray(im, np.float32) / 255.0


def region_pixels(img, box, kind):
    x0, y0, x1, y1 = box
    px = img[y0:y1, x0:x1].reshape(-1, 3)
    m = mask_of(px, kind)
    return px[m], float(m.mean())


def blade_profile(img, box):
    """Root / mid / tip means of the plant pixels (bottom / middle / top third of their vertical extent)."""
    x0, y0, x1, y1 = box
    crop = img[y0:y1, x0:x1]
    m = mask_of(crop.reshape(-1, 3), "plant").reshape(crop.shape[:2])
    rows = np.nonzero(m.any(1))[0]
    if len(rows) == 0:
        return None, m
    top, bot = rows.min(), rows.max() + 1
    out = {}
    for name, (a, b) in (("tip", (0, 1 / 3)), ("mid", (1 / 3, 2 / 3)), ("root", (2 / 3, 1))):
        r0 = top + int((bot - top) * a)
        r1 = top + int((bot - top) * b)
        sub = crop[r0:r1][m[r0:r1]]
        out[name] = stats(sub)
    # value profile in 16 bands, root (0) .. tip (15): the gradient plot of the blade comparison view
    prof = []
    for i in range(16):
        r1 = bot - int((bot - top) * i / 16)
        r0 = bot - int((bot - top) * (i + 1) / 16)
        sub = crop[r0:r1][m[r0:r1]]
        prof.append([round(float(x), 4) for x in (sub.mean(0) if len(sub) else np.zeros(3))])
    out["profile_root_to_tip"] = prof
    out["coverage"] = round(float(m.mean()), 3)
    return out, m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--print", action="store_true")
    a = ap.parse_args()
    cfg = json.load(open(os.path.join(REF, "regions.json")))
    imgs = {k: load(cfg, k) for k in cfg["images"]}
    res = {"regions": {}, "blades": {}}
    for r in cfg["regions"]:
        px, frac = region_pixels(imgs[r["image"]], r["box"], r["mask"])
        s = stats(px)
        if s:
            s["fraction"] = round(frac, 3)
            s["class"] = r["class"]
        res["regions"][r["id"]] = s
    tiles = []
    for b in cfg.get("blades", []):
        prof, m = blade_profile(imgs[b["image"]], b["box"])
        res["blades"][b["id"]] = dict(prof or {}, type=b["type"])
        x0, y0, x1, y1 = b["box"]
        crop = Image.fromarray((imgs[b["image"]][y0:y1, x0:x1] * 255).astype(np.uint8))
        w = max(1, round(crop.width * SHEET_H / crop.height))
        crop = crop.resize((w, SHEET_H), Image.LANCZOS)
        mk = Image.fromarray((m * 255).astype(np.uint8)).resize((w, SHEET_H), Image.NEAREST).convert("RGB")
        tiles.append((b["id"], crop, mk))
    if tiles:
        W = sum(t[1].width for t in tiles) + 8 * (len(tiles) + 1)
        sheet = Image.new("RGB", (W, SHEET_H * 2 + 40), (24, 24, 24))
        d = ImageDraw.Draw(sheet)
        x = 8
        for name, crop, mk in tiles:
            sheet.paste(crop, (x, 20))
            sheet.paste(mk, (x, 28 + SHEET_H))
            d.text((x, 4), name, fill=(230, 230, 230))
            x += crop.width + 8
        sheet.save(os.path.join(REF, "blade_sheet.png"))
    # JPEG copies for the in-game reference viewer (ground_ref: stb_image reads no WebP)
    for k, v in cfg["images"].items():
        out = os.path.join(REF, "view", k + ".jpg")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        Image.open(os.path.join(REF, v["file"])).convert("RGB").save(out, quality=92)
    with open(os.path.join(REF, "stats.json"), "w", newline="\n") as f:
        json.dump(res, f, indent=1)
        f.write("\n")
    if a.print:
        print("| Region | Class | Mask | Fraction | Mean sRGB | HSV mean | HSV std | V p5/50/95 |")
        print("|---|---|---|---|---|---|---|---|")
        for r in cfg["regions"]:
            s = res["regions"][r["id"]]
            if not s:
                continue
            print(f"| {r['id']} | {r['class']} | {r['mask']} | {s['fraction']} | `{s['hex']}` | {s['hsv_mean']} | {s['hsv_std']} | {s['v_p5_p50_p95']} |")
        print()
        print("| Blade crop | Type | Coverage | Root | Mid | Tip |")
        print("|---|---|---|---|---|---|")
        for k, v in res["blades"].items():
            if "root" not in v:
                continue
            f = lambda s: f"`{s['hex']}` {s['hsv_mean']}" if s else "-"
            print(f"| {k} | {v['type']} | {v['coverage']} | {f(v['root'])} | {f(v['mid'])} | {f(v['tip'])} |")


if __name__ == "__main__":
    main()
