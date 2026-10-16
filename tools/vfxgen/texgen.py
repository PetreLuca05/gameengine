"""Surface texture generator (docs/TEXTURES.md): terrain / building surfaces from Higgsfield images.

    py tools/vfxgen/texgen.py                 # generate missing images (cached), then process all
    py tools/vfxgen/texgen.py --only grass,mud
    py tools/vfxgen/texgen.py --process-only  # never calls the API
    py tools/vfxgen/texgen.py --regen grass   # pays again for a new image of these recipes

Per recipe (texgen_recipes.json): one `higgsfield-ai/soul/standard` image (1080p, ~$0.19), cached in
tools/vfxgen/cache/tex_<name>_<hash>/raw.png (git-ignored). Processing (free, deterministic):
  1. centre crop, colour flattening (removes the low-frequency light gradient of the photo);
  2. made tileable: separable variance-preserving blend with a half-period shifted copy (structured
     recipes keep an integer number of their pattern periods so rows / boards line up);
  3. outputs in assets/textures/surfaces/:
       <name>_albedo.jpg  1024 px sRGB albedo (JPEG q92: a tenth of the PNG)
       <name>_nrh.png     512 px: R,G tangent normal (0.5 = flat), B roughness, A height
Height comes from the luminance (band-passed), the normal from its gradient, the roughness from the
recipe's base value modulated by the cavity (darker, lower = rougher).
"""
import argparse
import glob
import json
import os
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "assets", "textures", "surfaces")
CACHE = os.path.join(HERE, "cache")
MODEL = "higgsfield-ai/soul/standard"

# Roughness base / height-to-normal strength per recipe (defaults: 0.85, 4.0).
ROUGH = {"asphalt": 0.8, "concrete": 0.8, "mud": 0.45, "metal": 0.55, "rust": 0.8, "sand": 0.9, "rock": 0.75, "plaster": 0.85, "wood": 0.75,
         "shingles": 0.85, "brick": 0.85}
BUMP = {"grass": 5.0, "meadow": 3.5, "forest": 5.0, "gravel": 6.0, "rock": 6.0, "brick": 5.0, "shingles": 5.0, "metal": 3.0, "plaster": 2.5,
        "concrete": 2.5, "sand": 2.0, "mud": 2.5, "asphalt": 2.0, "wood": 3.5}


def load_recipes():
    with open(os.path.join(HERE, "texgen_recipes.json"), encoding="utf-8") as f:
        d = json.load(f)
    return d["style"], d["recipes"]


def prompt_of(style, r):
    return r["prompt"] + ". " + style


def cache_dir_of(style, r):
    from hf_client import stable_key
    key = stable_key(MODEL, prompt_of(style, r), "1080p", "1:1")
    return os.path.join(CACHE, f"tex_{r['name']}_{key}"), key


# ------------------------------------------------------------------ generation

def generate(style, r, client, budget, regen=False):
    from hf_client import new_idempotency_key, usd_of
    cdir, key = cache_dir_of(style, r)
    os.makedirs(cdir, exist_ok=True)
    raw = os.path.join(cdir, "raw.png")
    if os.path.exists(raw) and not regen:
        return raw
    params = {"prompt": prompt_of(style, r), "num_images": 1, "resolution": "1080p", "aspect_ratio": "1:1"}
    est = client.estimate(MODEL, params, recipe="tex_" + r["name"])
    usd = usd_of(est)
    if usd is None:
        raise RuntimeError(f"{r['name']}: no USD in estimate {est}")
    if budget["spent"] + usd > budget["cap"]:
        raise RuntimeError(f"{r['name']}: ${usd:.2f} would exceed the cap (${budget['spent']:.2f} of ${budget['cap']:.2f})")
    idem = new_idempotency_key(f"tex/{key}/{int(regen and time.time())}")
    sub = client.submit(MODEL, params, idem, recipe="tex_" + r["name"])
    st = client.wait(sub, recipe="tex_" + r["name"])
    with open(os.path.join(cdir, "status.json"), "w", encoding="utf-8") as f:
        json.dump({"params": params, "estimate": est, "status": st}, f, indent=1)
    if str(st.get("status")).lower() != "completed":
        raise RuntimeError(f"{r['name']}: request ended '{st.get('status')}'")
    budget["spent"] += usd
    url = (st.get("images") or [{}])[0].get("url")
    if not url:
        raise RuntimeError(f"{r['name']}: no image in {st}")
    tmp = os.path.join(cdir, "raw_dl" + (os.path.splitext(url.split("?")[0])[1] or ".png"))
    client.download(url, tmp, recipe="tex_" + r["name"])
    Image.open(tmp).convert("RGB").save(raw)
    os.remove(tmp)
    print(f"  {r['name']}: generated (${usd:.3f}, run total ${budget['spent']:.2f})")
    return raw


# ------------------------------------------------------------------ processing

def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def blur_wrap(a, r):
    """Box blur (3 passes ~ gaussian) with wrap-around, radius r px, over the first two axes."""
    if r < 1:
        return a
    out = a.astype(np.float64)
    for _ in range(3):
        for ax in (0, 1):
            c = np.cumsum(np.concatenate([out.take(range(-r - 1, 0), axis=ax), out, out.take(range(0, r), axis=ax)], axis=ax), axis=ax)
            n = out.shape[ax]
            hi = c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax)
            lo = c.take(range(0, n), axis=ax)
            out = (hi - lo) / (2 * r + 1)
    return out


def blur_clamp(a, r):
    """Box blur (3 passes) with edge clamping (for the non-tileable source)."""
    if r < 1:
        return a
    out = a.astype(np.float64)
    for _ in range(3):
        for ax in (0, 1):
            n = out.shape[ax]
            first = out.take([0], axis=ax).repeat(r + 1, axis=ax)
            last = out.take([n - 1], axis=ax).repeat(r, axis=ax)
            c = np.cumsum(np.concatenate([first, out, last], axis=ax), axis=ax)
            out = (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return out


def period_of(lum, axis, lo=24, hi=None):
    """Dominant pattern period (px) along an axis from the autocorrelation of the mean profile, or 0."""
    prof = lum.mean(axis=1 - axis)
    prof = prof - prof.mean()
    n = len(prof)
    hi = hi or n // 3
    f = np.fft.rfft(prof, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    ac /= ac[0] + 1e-9
    best, bv = 0, 0.25
    for p in range(lo, hi):
        if ac[p] > ac[p - 1] and ac[p] >= ac[p + 1] and ac[p] > bv:
            best, bv = p, ac[p]
            break   # the first strong peak is the fundamental
    return best


def flatten_light(lin, frac=0.25, per_channel=False):
    """Divides out the low-frequency brightness (uneven light / vignetting / perspective), keeps the
    mean; per channel also flattens slow colour drifts (the seam cut then has nothing to step over)."""
    h = lin.shape[0]
    src = lin if per_channel else lin.mean(axis=2, keepdims=True)
    low = blur_clamp(src, max(4, int(h * frac / 2)))
    return np.clip(lin * (src.mean(axis=(0, 1), keepdims=True) / np.maximum(low, 1e-4)), 0, 1.5)


def seam_blend(img, axis, shift, width):
    """Tileable along `axis`: blend with a copy rolled by `shift`, weights a smooth ramp that is 1 on
    the original in the middle and 1 on the rolled copy at the borders; variance preserving
    (the mean is kept, the deviation divided by sqrt(a^2 + b^2))."""
    n = img.shape[axis]
    x = (np.arange(n) + 0.5) / n
    d = np.minimum(x, 1 - x)                        # 0 at the borders, 0.5 in the middle
    a = np.clip((d - 0.5 * (0.5 - width)) / width, 0, 1)
    a = a * a * (3 - 2 * a)
    shape = [1, 1, 1]
    shape[axis] = n
    a = a.reshape(shape)
    b = 1 - a
    rolled = np.roll(img, shift, axis=axis)
    mean = img.mean(axis=(0, 1), keepdims=True)
    mix = a * img + b * rolled
    return mean + (mix - mean) / np.sqrt(a * a + b * b)


def quilt_x(img, o, feather=3):
    """Tileable along x by self-quilting: the image loses o columns; its first o output columns
    take the right band (continuing the wrap) left of a minimum-error vertical cut and the left band
    right of it, so the only seam is one irregular line where both bands agree best."""
    h, w = img.shape[:2]
    left, right = img[:, :o], img[:, w - o:]
    e = ((left - right) ** 2).sum(axis=2)
    cost = e.copy()
    for y in range(1, h):
        prev = cost[y - 1]
        best = np.minimum(prev, np.minimum(np.concatenate([[np.inf], prev[:-1]]), np.concatenate([prev[1:], [np.inf]])))
        cost[y] += best
    cut = np.zeros(h, np.int64)
    cut[-1] = int(np.argmin(cost[-1]))
    for y in range(h - 2, -1, -1):
        c = cut[y + 1]
        lo, hi = max(c - 1, 0), min(c + 2, o)
        cut[y] = lo + int(np.argmin(cost[y, lo:hi]))
    # the cut must also meet itself over the vertical wrap (the y pass keeps rows aligned): ease the
    # last rows towards the first row's cut
    n_ease = min(48, h // 8)
    for k in range(n_ease):
        t = (k + 1) / (n_ease + 1)
        y = h - n_ease + k
        cut[y] = int(round(cut[y] * (1 - t) + cut[0] * t))
    cols = np.arange(o)[None, :]
    m = np.clip((cols - cut[:, None] + feather) / (2.0 * feather), 0, 1)[..., None]   # 0 right band, 1 left band
    band = right * (1 - m) + left * m
    return np.concatenate([band, img[:, o:w - o]], axis=1)


def make_tileable(lin, structured):
    n = lin.shape[0]
    lum = lin.mean(axis=2)
    out = lin
    if not structured:
        o = n // 8
        out = quilt_x(out, o)
        out = quilt_x(out.transpose(1, 0, 2), o).transpose(1, 0, 2)
        return np.clip(out, 0, 1)
    for axis in (1, 0):   # x then y
        shift = n // 2
        width = 0.08
        p = period_of(lum, axis)
        if p:
            shift = max(p, int(round((n // 2) / p)) * p)
        out = seam_blend(out, axis, shift, width)
    return np.clip(out, 0, 1)


def resize(arr, size):
    im = Image.fromarray(np.clip(arr * 255 + 0.5, 0, 255).astype(np.uint8))
    return np.asarray(im.resize((size, size), Image.LANCZOS)).astype(np.float64) / 255


def process(r, raw):
    name = r["name"]
    src = np.asarray(Image.open(raw).convert("RGB")).astype(np.float64) / 255
    h, w = src.shape[:2]
    s = min(h, w)
    if not r.get("period"):
        # the photos have some perspective (finer towards the top): a bottom-centred 60 % square has
        # the least scale change, so the quilting seams join similar detail
        s = int(s * 0.6)
        src = src[h - s:, (w - s) // 2:(w - s) // 2 + s]
    else:
        src = src[(h - s) // 2:(h - s) // 2 + s, (w - s) // 2:(w - s) // 2 + s]
    lin = srgb_to_lin(src)
    structured = bool(r.get("period"))
    lin = flatten_light(lin) if structured else flatten_light(lin, 0.07, True)
    if structured:
        # an integer number of pattern periods across the crop: rows / boards continue over the wrap
        lum = lin.mean(axis=2)
        for axis in (0, 1):
            p = period_of(lum, axis)
            if p:
                k = s // p
                keep = k * p
                if axis == 0:
                    lin = lin[(lin.shape[0] - keep) // 2:(lin.shape[0] - keep) // 2 + keep]
                else:
                    lin = lin[:, (lin.shape[1] - keep) // 2:(lin.shape[1] - keep) // 2 + keep]
        im = Image.fromarray(np.clip(lin_to_srgb(lin) * 255 + 0.5, 0, 255).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS)
        lin = srgb_to_lin(np.asarray(im).astype(np.float64) / 255)
    else:
        lin = srgb_to_lin(resize(lin_to_srgb(lin), 1170))   # - 2 x 1/8 quilting overlap = 1024
    lin = make_tileable(lin, structured)
    if lin.shape[0] != 1024 or lin.shape[1] != 1024:
        lin = srgb_to_lin(np.asarray(Image.fromarray(np.clip(lin_to_srgb(lin) * 255 + 0.5, 0, 255).astype(np.uint8)).resize((1024, 1024), Image.LANCZOS)).astype(np.float64) / 255)
    alb = lin_to_srgb(lin)
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(np.clip(alb * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(OUT, f"{name}_albedo.jpg"), quality=92, subsampling=0)
    # normal / roughness / height at 512
    small = srgb_to_lin(resize(alb, 512))
    lum = small @ np.array([0.2126, 0.7152, 0.0722])
    hgt = lum - blur_wrap(lum, 24)                 # band-pass: no large-scale slope in the height
    hgt = blur_wrap(hgt, 1)
    sd = hgt.std() + 1e-6
    hgt = np.clip(0.5 + hgt / (sd * 5.0), 0, 1)
    k = BUMP.get(name, 4.0)
    dx = (np.roll(hgt, -1, axis=1) - np.roll(hgt, 1, axis=1)) * 0.5 * k
    dy = (np.roll(hgt, -1, axis=0) - np.roll(hgt, 1, axis=0)) * 0.5 * k
    nz = 1.0 / np.sqrt(dx * dx + dy * dy + 1.0)
    nx, ny = -dx * nz, dy * nz                     # +y up in tangent space (image rows go down)
    rough = np.clip(ROUGH.get(name, 0.85) + (0.5 - hgt) * 0.25, 0.05, 1.0)
    nrh = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, rough, hgt], axis=2)
    Image.fromarray(np.clip(nrh * 255 + 0.5, 0, 255).astype(np.uint8)).save(os.path.join(OUT, f"{name}_nrh.png"), optimize=True)
    mean = lin.reshape(-1, 3).mean(axis=0)
    return mean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--regen", default="")
    ap.add_argument("--process-only", action="store_true")
    ap.add_argument("--cap", type=float, default=8.0, help="USD cap for this run")
    a = ap.parse_args()
    style, recipes = load_recipes()
    only = set(filter(None, a.only.split(",")))
    regen = set(filter(None, a.regen.split(",")))
    sel = [r for r in recipes if not only or r["name"] in only]
    budget = {"spent": 0.0, "cap": a.cap}
    client = None
    means = {}
    for r in sel:
        cdir, _ = cache_dir_of(style, r)
        raw = os.path.join(cdir, "raw.png")
        if not a.process_only and (not os.path.exists(raw) or r["name"] in regen):
            if client is None:
                from hf_client import Client
                client = Client()
            try:
                raw = generate(style, r, client, budget, regen=r["name"] in regen)
            except Exception as e:   # keep going: the other recipes still generate
                print(f"  {r['name']}: FAILED {e}")
                continue
        if not os.path.exists(raw):
            print(f"  {r['name']}: no cached image (run without --process-only)")
            continue
        means[r["name"]] = process(r, raw)
        print(f"  {r['name']}: processed, mean linear albedo {np.round(means[r['name']], 3).tolist()}")
    print(f"spent this run: ${budget['spent']:.2f}")


if __name__ == "__main__":
    main()
