#!/usr/bin/env python3
"""Ground texture checks (docs/GROUND_REFERENCE.md, ctest ground_textures). Runs on whatever set is in
assets/{foliage,terrain}/tex (placeholders or generated):

  blade atlases   per cell: alpha coverage in the recipe's range, blades reach the cell bottom, root / mid / tip
                  within 0.06 (sRGB, per channel) of the target gradient, hue of the atlas' plant pixels within
                  10 degrees of the reference region (ref_region), silhouette envelope IoU against the reference
                  crops (ref_blades: the hand-traced blades of regions.json, 8 x 8 fan envelopes) at or above `iou_min`
  surfaces        mean within 0.03 of target_mean, hue / saturation within 10 degrees / 0.15 of the reference
                  region, seamless (the wrap's line difference <= 1.25 x the 99th percentile of the interior ones;
                  a non-tiling photo scores 3 .. 5)
  scatter         every cell has a sprite (coverage 2 .. 70 %)

    py tools/tests/ground_tex_check.py [--verbose]
Exit code 1 on any failure.
"""
import glob
import json
import math
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "vfxgen"))
import ground_ref  # noqa: E402
import groundgen as GG  # noqa: E402

VERBOSE = "--verbose" in sys.argv
fails = []
checks = 0


def check(ok, msg):
    global checks
    checks += 1
    if not ok:
        fails.append(msg)
    elif VERBOSE:
        print("  ok  " + msg)


def hsv_of_hex(h):
    s = np.array([int(h.lstrip("#")[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])[None]
    return ground_ref.hsv(s)[0]


def hue_diff(a, b):
    d = abs(a - b) % 360
    return min(d, 360 - d)


def envelope(mask, n=8):
    """The fan shape: the silhouette cropped to its bounding box, n x n coverage (box filter), cells with
    any real coverage (> 4 %). Compares spread, height profile and arching, not blade positions."""
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return np.zeros((n, n), bool)
    crop = mask[ys.min():ys.max() + 1, xs.min():xs.max() + 1].astype(np.float32)
    im = Image.fromarray((crop * 255).astype(np.uint8)).resize((n, n), Image.BOX)
    return np.asarray(im).astype(np.float32) / 255 > 0.04


def traced_mask(b):
    """The hand-traced blades of a reference crop (regions.json 'traced') rasterised in the crop: tapered
    strokes (full width to 65 % of the length, then to a point)."""
    from PIL import ImageDraw
    x0, y0, x1, y1 = b["box"]
    ss = 4
    im = Image.new("L", ((x1 - x0) * ss, (y1 - y0) * ss), 0)
    d = ImageDraw.Draw(im)
    for t in b["traced"]:
        pts = np.array(t["pts"], np.float64)
        seg = np.hypot(*np.diff(pts, axis=0).T)
        tot = seg.sum()
        acc = 0.0
        for i in range(len(pts) - 1):
            for k in range(12):
                f = (acc + seg[i] * k / 12) / tot
                p = pts[i] + (pts[i + 1] - pts[i]) * k / 12
                w = t["w"] * (1.0 if f < 0.65 else max(0.15, 1 - (f - 0.65) / 0.35))
                r = w * ss / 2
                cx, cy = (p[0] - x0) * ss, (p[1] - y0) * ss
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
            acc += seg[i]
    return np.asarray(im.resize((x1 - x0, y1 - y0), Image.BOX)) > 127


def iou(a, b):
    u = (a | b).sum()
    return float((a & b).sum()) / max(1, u)


def main():
    recipes = GG.load_recipes()
    stats = json.load(open(os.path.join(ROOT, "docs", "reference", "ground", "stats.json")))
    regions = json.load(open(os.path.join(ROOT, "docs", "reference", "ground", "regions.json")))
    ref_imgs = {k: np.asarray(Image.open(os.path.join(ROOT, "docs", "reference", "ground", v["file"])).convert("RGB")).astype(np.float32) / 255
                for k, v in regions["images"].items()}
    blades = {b["id"]: b for b in regions["blades"]}
    for name, r in sorted(recipes.items()):
        out = os.path.join(GG.OUT_DIRS[r["_group"]], name)
        meta_p = out + ".meta.json"
        check(os.path.exists(meta_p), f"{name}: processed (meta.json present)")
        if not os.path.exists(meta_p):
            continue
        meta = json.load(open(meta_p))
        if r["kind"] == "blade_atlas":
            im = np.asarray(Image.open(out + "_albedo.png").convert("RGBA")).astype(np.float64) / 255
            check(os.path.exists(out + "_nta.png"), f"{name}: normal / thickness / AO map present")
            cols, rows = r["grid"]
            cw = im.shape[1] // cols
            lo, hi = r.get("checks", {}).get("coverage", [0.05, 0.5])
            for i, c in enumerate(r["cells"]):
                y, x = divmod(i, cols)
                cell = im[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw]
                a = cell[..., 3]
                m = a > 0.5
                cov = float(m.mean())
                check(lo <= cov <= hi, f"{name}[{i}]: coverage {cov:.3f} in [{lo}, {hi}]")
                check(m[int(cw * 0.97):].any(), f"{name}[{i}]: blades reach the cell bottom")
                tgt = dict(r.get("target", {}), **c.get("target", {}))
                # the green blades' gradient (dry pixels excluded by hue, as the colour match does)
                lin = GG.s2l(cell[..., :3])
                hue = GG.hue_of(lin)
                green = m & (hue >= 54) & (hue <= 110)   # flowers excluded
                rws = np.nonzero(green.any(1))[0]
                if len(rws) > 8 and green.sum() > 400:
                    top, bot = rws.min(), rws.max() + 1
                    for k, (p, q) in (("root", (5 / 6, 1)), ("mid", (5 / 12, 7 / 12)), ("tip", (0, 1 / 6))):
                        r0, r1 = top + int((bot - top) * p), top + int((bot - top) * q)
                        px = cell[r0:r1, :, :3][green[r0:r1]]
                        if len(px) < 50:
                            continue
                        got = px.mean(0)
                        want = np.array([int(tgt[k].lstrip("#")[j:j + 2], 16) / 255 for j in (0, 2, 4)])
                        d = float(np.abs(got - want).max())
                        check(d <= 0.07, f"{name}[{i}]: {k} {GG.lin_hex(GG.s2l(got))} within 0.07 of {tgt[k]} (delta {d:.3f})")
            # hue of the plant pixels against the reference region
            if r.get("ref_region"):
                plant = im[..., 3] > 0.5
                h = ground_ref.hsv(im[..., :3][plant])
                ang = np.radians(h[:, 0])
                hm = math.degrees(math.atan2(np.sin(ang).mean(), np.cos(ang).mean())) % 360
                ref = stats["regions"][r["ref_region"]]["hsv_mean"]
                check(hue_diff(hm, ref[0]) <= 10, f"{name}: plant hue {hm:.1f} within 10 of {r['ref_region']} ({ref[0]})")
            # silhouette envelope against the reference crops
            for bid in r.get("ref_blades", []):
                b = blades[bid]
                env_ref = envelope(traced_mask(b))
                best = 0.0
                for i in range(len(r["cells"])):
                    y, x = divmod(i, cols)
                    env = envelope(im[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw, 3] > 0.5)
                    best = max(best, iou(env, env_ref))
                need = r.get("iou_min", 0.45)
                check(best >= need, f"{name}: silhouette envelope IoU vs {bid} {best:.2f} >= {need}")
        elif r["kind"] == "scatter_atlas":
            im = np.asarray(Image.open(out + "_albedo.png").convert("RGBA")).astype(np.float64) / 255
            cols, rows = r["grid"]
            cw = im.shape[1] // cols
            for i in range(len(r["cells"])):
                y, x = divmod(i, cols)
                cov = float((im[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw, 3] > 0.5).mean())
                check(0.01 <= cov <= 0.7, f"{name}[{i}]: sprite coverage {cov:.3f}")
        else:
            im = np.asarray(Image.open(out + "_albedo.jpg").convert("RGB")).astype(np.float64) / 255
            for f in ("_nrh.png", "_ao.png", "_macro.png"):
                check(os.path.exists(out + f), f"{name}: {f} present")
            mean = GG.l2s(GG.s2l(im).reshape(-1, 3).mean(0))
            want = np.array([int(r["target_mean"].lstrip("#")[j:j + 2], 16) / 255 for j in (0, 2, 4)])
            d = float(np.abs(mean - want).max())
            check(d <= 0.03, f"{name}: mean {GG.lin_hex(GG.s2l(mean))} within 0.03 of {r['target_mean']} (delta {d:.3f})")
            seam = GG.seam_error(GG.s2l(im))
            check(seam <= 1.25, f"{name}: seamless (wrap / interior p99 line difference {seam:.2f} <= 1.25)")
            if r.get("ref_region"):
                th = hsv_of_hex(r["target_mean"])
                ref = stats["regions"][r["ref_region"]]["hsv_mean"]
                check(hue_diff(th[0], ref[0]) <= 10 and abs(th[1] - ref[1]) <= 0.15,
                      f"{name}: target {r['target_mean']} (h {th[0]:.0f} s {th[1]:.2f}) matches {r['ref_region']} (h {ref[0]:.0f} s {ref[1]:.2f})")
    print(f"ground_tex_check: {checks - len(fails)} / {checks} checks passed")
    for f in fails:
        print("  FAIL " + f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
