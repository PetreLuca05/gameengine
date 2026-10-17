"""Surface layer AO + metal masks (docs/TEXTURES.md "Ambient occlusion" / "Metal"), derived from the
existing layer images (no API calls, deterministic):

    py tools/vfxgen/surface_pbr.py              # all 20 layers
    py tools/vfxgen/surface_pbr.py --only brick,metal
    py tools/vfxgen/surface_pbr.py --sheet out.png   # + a contact sheet (albedo, AO, metal) to check

Inputs: assets/textures/surfaces/<name>_albedo.jpg and <name>_nrh.png (texgen.py). Outputs next to them:
  <name>_ao.png     512 px grey: horizon AO of the height channel (1 = open). surface_tex.c packs it
                    into the albedo array's alpha (no extra GPU memory).
  <name>_metal.png  512 px, metal layers only (metal, rust): R bare-metal mask (1 = bare / galvanised
                    steel, 0 = rust or grime), G paint wear (where paint on a painted prim is chipped
                    down to the metal). surface_tex.c appends them as extra layers of the nrh array.

AO: the height in texels is BUMP x the height channel (the same relief the normal map was derived
with, texgen.py), wrap-around (the layers tile). 16 directions, radii 1 .. 32 px; per direction the
highest elevation of the horizon (attenuated with distance), AO = 1 - mean sin(elevation), then a
gentle tone curve so open, flat areas stay at 1.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from texgen import BUMP, blur_wrap, srgb_to_lin   # noqa: E402  (the relief the normal maps use)

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SURF = os.path.join(ROOT, "assets", "textures", "surfaces")
LAYERS = ["grass", "meadow", "forest", "dirt", "rock", "sand", "gravel", "asphalt", "dirt_road", "concrete",
          "field", "crop", "plowed", "mud", "brick", "plaster", "wood", "metal", "rust", "shingles"]
METAL_LAYERS = ["metal", "rust"]
RADII = [1, 2, 3, 4, 6, 8, 11, 16, 22, 32]
DIRS = 16


def shift(a, dx, dy):
    """a sampled at (x + dx, y + dy), wrap-around, bilinear."""
    x0, y0 = int(np.floor(dx)), int(np.floor(dy))
    fx, fy = dx - x0, dy - y0
    r00 = np.roll(a, (-y0, -x0), axis=(0, 1))
    r10 = np.roll(a, (-y0, -x0 - 1), axis=(0, 1))
    r01 = np.roll(a, (-y0 - 1, -x0), axis=(0, 1))
    r11 = np.roll(a, (-y0 - 1, -x0 - 1), axis=(0, 1))
    return (r00 * (1 - fx) + r10 * fx) * (1 - fy) + (r01 * (1 - fx) + r11 * fx) * fy


def horizon_ao(h_tex, radii=RADII, dirs=DIRS):
    """Horizon AO of a tileable height field given in texels. 1 = unoccluded."""
    rmax = float(radii[-1])
    acc = np.zeros_like(h_tex)
    for d in range(dirs):
        a = (d + 0.5) * 2.0 * np.pi / dirs
        cx, cy = np.cos(a), np.sin(a)
        best = np.zeros_like(h_tex)
        for r in radii:
            dh = shift(h_tex, cx * r, cy * r) - h_tex
            s = dh / np.sqrt(dh * dh + r * r)                     # sin of the elevation
            s *= 1.0 - (r / (rmax * 1.25)) ** 2                    # far occluders count less
            best = np.maximum(best, s)
        acc += best
    return 1.0 - acc / dirs


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def value_noise(n, cells, seed):
    """Tileable value noise, n px, cells per side (bicubic-ish via smoothstep)."""
    rng = np.random.default_rng(seed)
    g = rng.random((cells, cells))
    y, x = np.mgrid[0:n, 0:n] * (cells / n)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = smoothstep(0, 1, x - x0), smoothstep(0, 1, y - y0)
    x1, y1 = (x0 + 1) % cells, (y0 + 1) % cells
    x0, y0 = x0 % cells, y0 % cells
    return (g[y0, x0] * (1 - fx) + g[y0, x1] * fx) * (1 - fy) + (g[y1, x0] * (1 - fx) + g[y1, x1] * fx) * fy


def fbm(n, seed):
    return (value_noise(n, 8, seed) * 0.5 + value_noise(n, 16, seed + 1) * 0.3 + value_noise(n, 32, seed + 2) * 0.2)


def load_layer(name):
    alb = np.asarray(Image.open(os.path.join(SURF, f"{name}_albedo.jpg")).convert("RGB").resize((512, 512), Image.LANCZOS)).astype(np.float64) / 255
    nrh = np.asarray(Image.open(os.path.join(SURF, f"{name}_nrh.png")).convert("RGBA")).astype(np.float64) / 255
    return alb, nrh


def bake_ao(name, nrh):
    h = nrh[..., 3] * BUMP.get(name, 4.0)
    ao = horizon_ao(h)
    # tone: the median (open surface) maps to ~1, the cavities keep their depth
    med = np.median(ao)
    ao = np.clip(ao / max(med, 1e-3), 0.0, 1.0) ** 1.5
    return ao


def bake_metal(name, alb, nrh):
    """R bare metal, G paint wear (both 0..1)."""
    lin = srgb_to_lin(alb)
    lum = lin @ np.array([0.2126, 0.7152, 0.0722])
    mx, mn = alb.max(axis=2), alb.min(axis=2)
    sat = (mx - mn) / np.maximum(mx, 1e-3)
    orange = smoothstep(0.0, 0.08, alb[..., 0] - alb[..., 2])   # rust is red over blue
    hgt = nrh[..., 3]
    n = alb.shape[0]
    if name == "metal":
        # galvanised sheet: bare everywhere except rust spots (saturated orange) and heavy grime
        rust = smoothstep(0.12, 0.3, sat) * orange
        grime = 1.0 - smoothstep(0.03, 0.12, lum)
        bare = np.clip(1.0 - rust - 0.6 * grime, 0.0, 1.0)
        # paint wear: bright fine scratches + chips on the corrugation crests, in noise patches
        hp = lum - blur_wrap(lum, 3)
        scratch = smoothstep(0.015, 0.05, hp)
        crest = smoothstep(0.62, 0.8, hgt) * smoothstep(0.55, 0.75, fbm(n, 11))
        chips = smoothstep(0.72, 0.8, fbm(n, 23))
        wear = np.clip(np.maximum.reduce([scratch, crest, chips]), 0.0, 1.0) * bare
    else:
        # rust: the photo is patina everywhere, so the metal patches are where it is thinnest: the least
        # saturated, smoothest (least relief) regions, in noise patches (about a fifth of the surface)
        sat_s = blur_wrap(sat, 4)
        relief = np.sqrt(blur_wrap((hgt - blur_wrap(hgt, 3)) ** 2, 4))
        thin = (np.percentile(sat_s, 50) - sat_s) / max(np.percentile(sat_s, 50) - np.percentile(sat_s, 5), 1e-3)
        thin += (np.percentile(relief, 50) - relief) / max(np.percentile(relief, 50) - np.percentile(relief, 5), 1e-3)
        thin = 0.5 * thin + 0.8 * (fbm(n, 37) - 0.5)
        bare = smoothstep(0.35, 0.8, thin) * smoothstep(0.02, 0.06, lum)
        wear = bare   # a painted rusty prim (cranes, hulls) shows its metal where the patina does
    return bare, wear


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--sheet", default="")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    rows = []
    for name in LAYERS:
        if only and name not in only:
            continue
        alb, nrh = load_layer(name)
        ao = bake_ao(name, nrh)
        Image.fromarray(np.clip(ao * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(SURF, f"{name}_ao.png"), optimize=True)
        info = f"AO mean {ao.mean():.3f}, p05 {np.percentile(ao, 5):.3f}"
        bare = wear = None
        if name in METAL_LAYERS:
            bare, wear = bake_metal(name, alb, nrh)
            mt = np.stack([bare, wear, np.zeros_like(bare)], axis=2)
            Image.fromarray(np.clip(mt * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(SURF, f"{name}_metal.png"), optimize=True)
            info += f", bare metal {bare.mean():.2f}, paint wear {wear.mean():.2f}"
        print(f"  {name}: {info}")
        rows.append((name, alb, ao, bare, wear))
    if a.sheet and rows:
        w = 256
        sheet = Image.new("RGB", (w * 4, w * len(rows)))
        for i, (name, alb, ao, bare, wear) in enumerate(rows):
            sheet.paste(Image.fromarray((alb * 255).astype(np.uint8)).resize((w, w)), (0, i * w))
            sheet.paste(Image.fromarray((ao * 255).astype(np.uint8)).convert("RGB").resize((w, w)), (w, i * w))
            if bare is not None:
                sheet.paste(Image.fromarray((bare * 255).astype(np.uint8)).convert("RGB").resize((w, w)), (2 * w, i * w))
                sheet.paste(Image.fromarray((wear * 255).astype(np.uint8)).convert("RGB").resize((w, w)), (3 * w, i * w))
        sheet.save(a.sheet)


if __name__ == "__main__":
    main()
