#!/usr/bin/env python3
"""groundgen - the ground texture set (docs/GROUND_REFERENCE.md): grass blade atlases, plant cards,
terrain layers + variants, transition sets, scatter atlases. One command does everything:

    py tools/vfxgen/groundgen.py                 generate what is missing (paid, cached), process all, check, report
    py tools/vfxgen/groundgen.py --dry-run       list the requests and the estimated cost, call nothing
    py tools/vfxgen/groundgen.py --placeholder   procedural set only (free, no key needed)
    py tools/vfxgen/groundgen.py --process-only  re-process the raw images already here (free)
    py tools/vfxgen/groundgen.py --only grass_broad,dirt     some recipes
    py tools/vfxgen/groundgen.py --regen grass_broad         pay again for these recipes
    py tools/vfxgen/groundgen.py --validate      check the recipes only (no images; ctest runs this)
    options: --max-usd N (default 15), --iterate N (re-rolls of cells that fail their checks, default 2),
             --commit / --push (stage only the generated paths, commit, push the current branch)

    groundgen.bat (repo root) = generate + process + commit + push: the one command for a machine with the key.

Recipes: assets/foliage/gen/*.json (blade / card atlases), assets/terrain/gen/*.json (layers, transitions,
scatter). Files starting with "_" hold the shared style lines. Kinds:

  blade_atlas    grid of card cells, one generated image per cell: blades (or a plant) on a flat magenta
                 backdrop, keyed, anchored to the cell's bottom edge, colour-matched root -> mid -> tip to the
                 recipe's target (green and dry blades separately, by hue), + normal / thickness / AO.
                 -> assets/foliage/tex/<name>_albedo.png (sRGB RGBA), <name>_nta.png (normal xy, thickness, AO)
  terrain_layer  one tileable surface (self-quilted, low frequencies flattened), mean matched to target_mean
  transition     same processing as a layer; blended inside a transition band by the terrain shader
                 -> assets/terrain/tex/<name>_albedo.jpg (2048), <name>_nrh.png (1024: normal xy, rough, height),
                    <name>_ao.png (1024), <name>_macro.png (512, tileable macro colour / height variation)
  scatter_atlas  grid of sprites (pebbles, leaves, twigs, grass bits, cracks) on magenta, keyed, centred
                 -> assets/terrain/tex/<name>_albedo.png (RGBA), <name>_nrh.png

Raw images: generated once (cached in tools/vfxgen/cache/ground_*, git-ignored) and mirrored to
tools/vfxgen/ground_raw/<name>_<cell>.jpg, which is committed: processing can be re-run anywhere without the
key. A cell with no raw image falls back to its procedural placeholder (ground_placeholder.py), so a partly
generated set still builds. Every output gets a <name>.meta.json (source per cell, checks, colours).

The key comes from the project .env (OPEN_HIGGSFIELD_READ_WRITE_TOKEN) or the environment; it is never
printed. API calls are logged (redacted) to tools/vfxgen/logs/api_log.jsonl.
"""
import argparse
import glob
import json
import math
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ground_placeholder as PH  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GEN_DIRS = {"foliage": os.path.join(ROOT, "assets", "foliage", "gen"), "terrain": os.path.join(ROOT, "assets", "terrain", "gen")}
OUT_DIRS = {"foliage": os.path.join(ROOT, "assets", "foliage", "tex"), "terrain": os.path.join(ROOT, "assets", "terrain", "tex")}
CACHE = os.path.join(HERE, "cache")
RAW = os.path.join(HERE, "ground_raw")
REPORT = os.path.join(ROOT, "docs", "ground_match", "gen_report.md")
SHEET = os.path.join(ROOT, "docs", "ground_match", "gen_sheet_{}.png")
DEFAULT_MODEL = "higgsfield-ai/soul/standard"
KINDS = ("blade_atlas", "terrain_layer", "transition", "scatter_atlas")
TOOL_VERSION = 1


# ------------------------------------------------------------------ colour helpers

def s2l(c):
    c = np.asarray(c, np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def l2s(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def hex_lin(h):
    return PH.hex_lin(h)


def lin_hex(c):
    s = l2s(np.asarray(c))
    return "#%02x%02x%02x" % tuple(int(round(float(x) * 255)) for x in s)


def hue_of(lin):
    """Hue in degrees of linear RGB (N, 3) or (..., 3) via sRGB."""
    s = l2s(lin)
    mx, mn = s.max(-1), s.min(-1)
    d = np.maximum(mx - mn, 1e-6)
    r, g, b = s[..., 0], s[..., 1], s[..., 2]
    h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4))
    return h * 60.0


def save_srgb(path, lin, alpha=None, quality=92):
    s = np.clip(l2s(lin) * 255 + 0.5, 0, 255).astype(np.uint8)
    if alpha is not None:
        s = np.concatenate([s, np.clip(alpha * 255 + 0.5, 0, 255).astype(np.uint8)[..., None]], axis=2)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im = Image.fromarray(s)
    if path.endswith(".jpg"):
        im.save(path, quality=quality, subsampling=0)
    else:
        im.save(path, optimize=True)


def save_lin(path, arr):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray(np.clip(arr * 255 + 0.5, 0, 255).astype(np.uint8)).save(path, optimize=True)


def resize_lin(lin, w, h=None):
    """Resample in sRGB space with Lanczos (as texgen does), channels 3 or 4 (alpha resampled linearly)."""
    h = h or w
    if lin.shape[2] == 4:
        a = lin[..., 3]
        pre = np.concatenate([l2s(lin[..., :3]) * a[..., None], a[..., None]], axis=2)
        im = Image.fromarray(np.clip(pre * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").resize((w, h), Image.LANCZOS)
        out = np.asarray(im).astype(np.float64) / 255
        aa = out[..., 3:]
        col = out[..., :3] / np.maximum(aa, 1e-4)
        return np.concatenate([s2l(np.clip(col, 0, 1)), aa], axis=2)
    im = Image.fromarray(np.clip(l2s(lin) * 255 + 0.5, 0, 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)
    return s2l(np.asarray(im).astype(np.float64) / 255)


def blur(a, r, wrap=False):
    """3-pass box blur over the first two axes, radius r px."""
    if r < 1:
        return a
    out = a.astype(np.float64)
    for _ in range(3):
        for ax in (0, 1):
            n = out.shape[ax]
            if wrap:
                pad_lo = out.take(range(-r - 1, 0), axis=ax)
                pad_hi = out.take(range(0, r), axis=ax)
            else:
                pad_lo = out.take([0], axis=ax).repeat(r + 1, axis=ax)
                pad_hi = out.take([n - 1], axis=ax).repeat(r, axis=ax)
            c = np.cumsum(np.concatenate([pad_lo, out, pad_hi], axis=ax), axis=ax)
            out = (c.take(range(2 * r + 1, 2 * r + 1 + n), axis=ax) - c.take(range(0, n), axis=ax)) / (2 * r + 1)
    return out


def erode(m, r):
    out = m.copy()
    for _ in range(r):
        out = out & np.roll(out, 1, 0) & np.roll(out, -1, 0) & np.roll(out, 1, 1) & np.roll(out, -1, 1)
    return out


def dilate(m, r):
    out = m.copy()
    for _ in range(r):
        out = out | np.roll(out, 1, 0) | np.roll(out, -1, 0) | np.roll(out, 1, 1) | np.roll(out, -1, 1)
    return out


# ------------------------------------------------------------------ recipes

def load_styles(group):
    p = os.path.join(GEN_DIRS[group], "_style.json")
    if not os.path.exists(p):
        return {}
    with open(p, encoding="utf-8") as f:
        return json.load(f).get("styles", {})


def load_recipes():
    out = {}
    for group, d in GEN_DIRS.items():
        styles = load_styles(group)
        for path in sorted(glob.glob(os.path.join(d, "*.json"))):
            if os.path.basename(path).startswith("_"):
                continue
            with open(path, encoding="utf-8") as f:
                r = json.load(f)
            r.setdefault("name", os.path.splitext(os.path.basename(path))[0])
            r["_group"] = group
            r["_path"] = path
            r["_style"] = styles.get(r.get("style", ""), "")
            out[r["name"]] = r
    return out


def cells_of(r):
    """The generation units of a recipe: atlas cells, or one image for a surface."""
    if r["kind"] in ("blade_atlas", "scatter_atlas"):
        return r["cells"]
    return [{"prompt": r["prompt"], "placeholder": r.get("placeholder", {})}]


def prompt_of(r, cell):
    p = cell["prompt"]
    if r.get("prompt_prefix"):
        p = r["prompt_prefix"] + " " + p
    if r["_style"]:
        p = p + ". " + r["_style"]
    neg = cell.get("avoid", r.get("avoid"))
    if neg:
        p = p + ". Avoid: " + neg   # soul/standard takes no negative prompt: the avoid list goes in the text
    return p


def gen_params(r, cell, attempt):
    p = prompt_of(r, cell)
    if attempt:
        p += f" (variation {attempt + 1})"
    return {"prompt": p, "num_images": 1, "resolution": r.get("resolution", "1080p"), "aspect_ratio": r.get("aspect_ratio", "1:1")}


def validate(recipes):
    errs = []
    names = set()
    for name, r in recipes.items():
        where = os.path.relpath(r["_path"], ROOT)
        if r.get("kind") not in KINDS:
            errs.append(f"{where}: kind must be one of {KINDS}")
            continue
        if name in names:
            errs.append(f"{where}: duplicate name {name}")
        names.add(name)
        if r["kind"] in ("blade_atlas", "scatter_atlas"):
            g = r.get("grid")
            if not (isinstance(g, list) and len(g) == 2 and g[0] * g[1] == len(r.get("cells", []))):
                errs.append(f"{where}: grid {g} must hold exactly the {len(r.get('cells', []))} cells")
            for i, c in enumerate(r.get("cells", [])):
                if not c.get("prompt"):
                    errs.append(f"{where}: cell {i} has no prompt")
                if "placeholder" not in c:
                    errs.append(f"{where}: cell {i} has no placeholder")
            if r.get("background", "magenta") not in ("magenta", "white", "black"):
                errs.append(f"{where}: background must be magenta / white / black")
            if r["kind"] == "blade_atlas":
                t = r.get("target", {})
                for k in ("root", "mid", "tip"):
                    if k not in t:
                        errs.append(f"{where}: target.{k} missing")
                chk = r.get("checks", {})
                cov = chk.get("coverage", [0.05, 0.6])
                if not (0 <= cov[0] < cov[1] <= 1):
                    errs.append(f"{where}: checks.coverage must be [lo, hi] in 0..1")
        else:
            if not r.get("prompt"):
                errs.append(f"{where}: prompt missing")
            if "target_mean" not in r:
                errs.append(f"{where}: target_mean missing")
            if r.get("tile_m", 0) <= 0:
                errs.append(f"{where}: tile_m must be > 0")
            if r["kind"] == "transition" and not (isinstance(r.get("pair"), list) and len(r["pair"]) == 2):
                errs.append(f"{where}: transition needs pair [a, b]")
        for k in ("target_mean",):
            if k in r:
                try:
                    hex_lin(r[k])
                except Exception:
                    errs.append(f"{where}: {k} is not a #rrggbb colour")
        if "model" not in r:
            errs.append(f"{where}: model missing (use {DEFAULT_MODEL})")
    return errs


# ------------------------------------------------------------------ generation (paid, cached)

def raw_path(r, i):
    return os.path.join(RAW, f"{r['name']}_{i}.jpg")


def cache_dir(r, i, attempt):
    from hf_client import stable_key
    key = stable_key(r.get("model", DEFAULT_MODEL), gen_params(r, cells_of(r)[i], attempt))
    return os.path.join(CACHE, f"ground_{r['name']}_{i}_{key}")


def generate_cell(r, i, client, budget, attempt, regen):
    """One image for cell i (attempt > 0 = a re-roll with another variation). Returns the raw path."""
    from hf_client import new_idempotency_key, usd_of
    cdir = cache_dir(r, i, attempt)
    raw = os.path.join(cdir, "raw.png")
    if os.path.exists(raw) and not regen:
        _mirror(raw, r, i)
        return raw
    os.makedirs(cdir, exist_ok=True)
    model = r.get("model", DEFAULT_MODEL)
    params = gen_params(r, cells_of(r)[i], attempt)
    tag = f"ground_{r['name']}_{i}"
    est = client.estimate(model, params, recipe=tag)
    usd = usd_of(est)
    if usd is None:
        raise RuntimeError(f"{tag}: no USD in the estimate {est}")
    if budget["spent"] + usd > budget["cap"]:
        raise RuntimeError(f"{tag}: ${usd:.2f} would exceed the cap (${budget['spent']:.2f} of ${budget['cap']:.2f}); raise --max-usd")
    idem = new_idempotency_key(f"ground/{os.path.basename(cdir)}/{int(regen and time.time())}")
    sub = client.submit(model, params, idem, recipe=tag)
    st = client.wait(sub, recipe=tag)
    with open(os.path.join(cdir, "status.json"), "w", encoding="utf-8") as f:
        json.dump({"params": params, "estimate": est, "status": st}, f, indent=1)
    if str(st.get("status")).lower() != "completed":
        raise RuntimeError(f"{tag}: request ended '{st.get('status')}'")
    budget["spent"] += usd
    budget["images"] += 1
    url = (st.get("images") or [{}])[0].get("url")
    if not url:
        raise RuntimeError(f"{tag}: no image in {st}")
    tmp = os.path.join(cdir, "raw_dl" + (os.path.splitext(url.split("?")[0])[1] or ".png"))
    client.download(url, tmp, recipe=tag)
    Image.open(tmp).convert("RGB").save(raw)
    os.remove(tmp)
    _mirror(raw, r, i)
    print(f"  {tag}: generated (${usd:.3f}, run total ${budget['spent']:.2f})")
    return raw


def _mirror(raw, r, i):
    os.makedirs(RAW, exist_ok=True)
    Image.open(raw).convert("RGB").save(raw_path(r, i), quality=95, subsampling=0)


def load_raw(r, i):
    p = raw_path(r, i)
    if not os.path.exists(p):
        return None
    return s2l(np.asarray(Image.open(p).convert("RGB")).astype(np.float64) / 255)


# ------------------------------------------------------------------ keying (atlases)

KEYS = {"magenta": (1.0, 0.0, 1.0), "white": (1.0, 1.0, 1.0), "black": (0.0, 0.0, 0.0)}


def key_out(r, lin):
    """Straight-alpha RGBA (linear) from a flat backdrop photo. The backdrop colour is measured on the
    border (generated screens are never pure); spill toward magenta is clamped (B <= G, R <= max(G, 1.25 G))."""
    srgb = l2s(lin)
    n = 10
    border = np.concatenate([srgb[:n].reshape(-1, 3), srgb[-n:].reshape(-1, 3), srgb[:, :n].reshape(-1, 3), srgb[:, -n:].reshape(-1, 3)])
    bg = np.median(border, axis=0)
    ref = np.array(KEYS.get(r.get("background", "magenta")))
    if np.linalg.norm(bg - ref) > 0.45:
        bg = ref
    spread = float(np.percentile(np.linalg.norm(border - bg, axis=1), 90))
    al = r.get("alpha", {})
    tol = al.get("tol", 0.12) + spread * 0.5
    soft = al.get("soft", 0.10)
    d = np.sqrt(((srgb - bg) ** 2).sum(axis=2)) / math.sqrt(3.0)
    a = np.clip((d - tol) / soft, 0, 1)
    col = srgb.copy()
    if r.get("background", "magenta") == "magenta" and r.get("despill", True):
        g = col[..., 1]
        col[..., 2] = np.minimum(col[..., 2], np.maximum(g, col[..., 2] * (1 - a) + g * a))
        col[..., 0] = np.minimum(col[..., 0], np.maximum(g * 1.25, g + 0.08))
    # un-mix the backdrop from the soft edge
    col = np.clip((col - (1 - a[..., None]) * bg) / np.maximum(a[..., None], 0.08), 0, 1)
    # specks: open the mask (erode then dilate) and keep only what survives
    solid = a > 0.5
    keep = dilate(erode(solid, 1), 2)
    a = a * keep
    return np.concatenate([s2l(col), a[..., None]], axis=2)


def anchor_bottom(rgba, size, fill=0.94, pad_x=0.03):
    """Blades anchored to the cell's bottom edge: crop to the alpha bbox, scale so the plant fills `fill`
    of the cell height (or its width fits), bottom-centred; the lowest rows touch the cell bottom."""
    a = rgba[..., 3]
    ys, xs = np.nonzero(a > 0.08)
    if len(xs) == 0:
        return np.zeros((size, size, 4))
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    crop = rgba[y0:y1, x0:x1]
    h, w = crop.shape[:2]
    s = min(size * fill / h, size * (1 - 2 * pad_x) / w)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    crop = resize_lin(crop, nw, nh)
    out = np.zeros((size, size, 4))
    ox = (size - nw) // 2
    out[size - nh:, ox:ox + nw] = crop
    return out


def center_fit(rgba, size, pad=0.08):
    a = rgba[..., 3]
    ys, xs = np.nonzero(a > 0.08)
    if len(xs) == 0:
        return np.zeros((size, size, 4))
    crop = rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h, w = crop.shape[:2]
    s = size * (1 - 2 * pad) / max(h, w)
    nw, nh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    crop = resize_lin(crop, nw, nh)
    out = np.zeros((size, size, 4))
    oy, ox = (size - nh) // 2, (size - nw) // 2
    out[oy:oy + nh, ox:ox + nw] = crop
    return out


# ------------------------------------------------------------------ blade processing

def gradient_match(cell, target, strength, dry_hue=54.0, bands=16):
    """Per height band gains so the green blades (hue >= dry_hue) follow root -> mid -> tip of the target
    and the dry ones dry_root -> dry_mid -> dry_tip. Means are measured on the plant pixels of the cell, in
    linear RGB; gains are smoothed over the bands and limited to 0.4 .. 2.5. strength 0 keeps the source."""
    rgb, a = cell[..., :3], cell[..., 3]
    n = cell.shape[0]
    m = a > 0.5
    rows = np.nonzero(m.any(1))[0]
    if len(rows) < 4 or strength <= 0:
        return cell
    top, bot = rows.min(), rows.max() + 1
    hue = hue_of(rgb)
    w_dry = PH.smooth(dry_hue + 3, dry_hue - 3, hue)
    out = np.zeros_like(rgb)
    for cls, keys in (("green", ("root", "mid", "tip")), ("dry", ("dry_root", "dry_mid", "dry_tip"))):
        cw = w_dry if cls == "dry" else 1 - w_dry
        if keys[0] not in target:
            out += rgb * cw[..., None]
            continue
        tc = [hex_lin(target[k]) for k in keys]
        gains = np.ones((bands, 3))
        valid = np.zeros(bands, bool)
        for b in range(bands):
            r1 = bot - int((bot - top) * b / bands)
            r0 = bot - int((bot - top) * (b + 1) / bands)
            ww = (m[r0:r1] * cw[r0:r1])
            if ww.sum() < 20:
                continue
            mean = (rgb[r0:r1] * ww[..., None]).sum((0, 1)) / ww.sum()
            t = (b + 0.5) / bands
            want = tc[0] * (1 - PH.smooth(0, 0.5, t)) + tc[1] * PH.smooth(0, 0.5, t) * (1 - PH.smooth(0.5, 1, t)) + tc[2] * PH.smooth(0.5, 1, t)
            gains[b] = np.clip(want / np.maximum(mean, 1e-4), 0.4, 2.5)
            valid[b] = True
        if not valid.any():
            out += rgb * cw[..., None]
            continue
        idx = np.arange(bands)
        for ch in range(3):
            gains[:, ch] = np.interp(idx, idx[valid], gains[valid, ch])
        gains = np.array([np.convolve(np.pad(gains[:, ch], 2, mode="edge"), np.ones(5) / 5, mode="valid") for ch in range(3)]).T
        # per row gain (row -> band, linear interpolation)
        yy = np.arange(n)
        tb = np.clip((bot - yy - 0.5) / max(1, bot - top) * bands - 0.5, 0, bands - 1)
        grow = np.stack([np.interp(tb, idx, gains[:, ch]) for ch in range(3)], 1)
        g = 1 + (grow[:, None, :] - 1) * strength
        out += np.clip(rgb * g, 0, 1) * cw[..., None]
    return np.concatenate([np.clip(out, 0, 1), cell[..., 3:]], axis=2)


def blade_maps(cell, rib_px=1.5):
    """Normal (dome across each blade from the alpha distance), thickness (alpha mass, lower for dry), AO
    (root darkening + overlap)."""
    a = cell[..., 3]
    n = a.shape[0]
    # dome height: blurs of the solid mask, masked (0 at the edges, highest on the blade centre line)
    solid = (a > 0.5).astype(np.float64)
    h = blur((blur(solid, 1) + blur(solid, 2) + blur(solid, 4)) / 3 * solid, 1)
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    k = 2.5
    nz = 1 / np.sqrt((gx * k) ** 2 + (gy * k) ** 2 + 1)
    nx, ny = -gx * k * nz, gy * k * nz
    lum = cell[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
    hue = hue_of(cell[..., :3])
    dryness = PH.smooth(58, 48, hue)
    thick = np.clip(a * (0.9 - 0.45 * dryness) * (0.85 + 0.3 * np.clip(lum * 3, 0, 1)), 0, 1)
    yy = (np.arange(n) / (n - 1))[:, None] * np.ones((1, n))
    root = PH.smooth(1.0, 0.72, yy)   # 0 at the bottom row, 1 from 28 % up
    overlap = blur(solid, 6)
    ao = np.clip((0.55 + 0.45 * root) * (1 - 0.25 * np.clip(overlap - 0.35, 0, 1)), 0, 1)
    nta = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, thick, ao], 2)
    return nta


def bleed(rgba, passes=8):
    """Colour under transparent texels from the nearest blade colour (no dark mip fringes)."""
    col = rgba[..., :3].copy()
    known = rgba[..., 3] > 0.05
    for _ in range(passes):
        acc = np.zeros_like(col)
        cnt = np.zeros(known.shape)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            acc += np.roll(np.roll(col, dy, 0), dx, 1) * k[..., None]
            cnt += k
        fill = (~known) & (cnt > 0)
        col[fill] = acc[fill] / cnt[fill][..., None]
        known |= fill
    # whatever is left: the mean plant colour
    if (~known).any():
        m = rgba[..., 3] > 0.5
        col[~known] = col[m].mean(0) if m.any() else 0.2
    return np.concatenate([col, rgba[..., 3:]], axis=2)


def cell_metrics(cell):
    a = cell[..., 3]
    m = a > 0.5
    n = a.shape[0]
    rows = np.nonzero(m.any(1))[0]
    cov = float(m.mean())
    bottom = float(m[int(n * 0.97):].mean())   # blades reach the cell bottom
    if len(rows) == 0:
        return {"coverage": 0, "bottom": 0}
    top, bot = rows.min(), rows.max() + 1
    out = {"coverage": round(cov, 3), "bottom": round(bottom, 3), "height": round((bot - top) / n, 3)}
    for name, (p, q) in (("root", (2 / 3, 1)), ("mid", (1 / 3, 2 / 3)), ("tip", (0, 1 / 3))):
        r0, r1 = top + int((bot - top) * p), top + int((bot - top) * q)
        sub = cell[r0:r1, :, :3][m[r0:r1]]
        if len(sub):
            out[name] = lin_hex(sub.mean(0))
    # magenta residue: plant pixels with B > G + 0.05 (sRGB) and R > G
    s = l2s(cell[..., :3])
    out["residue"] = round(float(((s[..., 2] > s[..., 1] + 0.05) & (s[..., 0] > s[..., 1]) & m).sum() / max(1, m.sum())), 4)
    return out


def check_cell(r, met):
    chk = r.get("checks", {})
    lo, hi = chk.get("coverage", [0.05, 0.6])
    fails = []
    if not lo <= met.get("coverage", 0) <= hi:
        fails.append(f"coverage {met.get('coverage')} not in [{lo}, {hi}]")
    if met.get("bottom", 0) < chk.get("bottom", 0.02):
        fails.append(f"blades do not reach the bottom ({met.get('bottom')})")
    if met.get("residue", 0) > chk.get("residue", 0.03):
        fails.append(f"backdrop residue {met.get('residue')}")
    return fails


def process_blade_atlas(r, raws, placeholder_only):
    cols, rows = r["grid"]
    size = r.get("size", 2048)
    cw = size // cols
    atlas = np.zeros((rows * cw, cols * cw, 4))
    meta_cells = []
    for i, c in enumerate(r["cells"]):
        src = None if placeholder_only else raws.get(i)
        if src is not None:
            keyed = key_out(r, src)
            cell = anchor_bottom(keyed, cw, fill=c.get("fill", r.get("fill", 0.94)))
            cell = gradient_match(cell, dict(r.get("target", {}), **c.get("target", {})), r.get("color_match", 0.85))
            source = "generated"
        else:
            spec = dict(c["placeholder"])
            spec["target"] = dict(r.get("target", {}), **c.get("target", {}))
            cell = PH.blade_cell(spec, cw, c["placeholder"].get("seed", i + 1))
            cell = gradient_match(cell, spec["target"], r.get("color_match", 0.85))
            source = "placeholder"
        met = cell_metrics(cell)
        meta_cells.append(dict(met, source=source, fails=check_cell(r, met) if source == "generated" else []))
        y, x = divmod(i, cols)
        atlas[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw] = cell
    atlas = bleed(atlas)
    nta = _per_cell(atlas, cols, rows, cw, blade_maps)
    out = os.path.join(OUT_DIRS[r["_group"]], r["name"])
    save_srgb(out + "_albedo.png", atlas[..., :3], atlas[..., 3])
    save_lin(out + "_nta.png", nta)
    return {"grid": [cols, rows], "size": size, "cells": meta_cells}


def _per_cell(atlas, cols, rows, cw, fn):
    out = np.zeros(atlas.shape[:2] + (4,))
    for y in range(rows):
        for x in range(cols):
            out[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw] = fn(atlas[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw])
    return out


# ------------------------------------------------------------------ surfaces (layers, transitions)

def flatten_light(lin, frac=0.07):
    h = lin.shape[0]
    low = blur(lin, max(4, int(h * frac / 2)))
    return np.clip(lin * (lin.mean(axis=(0, 1), keepdims=True) / np.maximum(low, 1e-4)), 0, 1.5)


def make_tileable(lin):
    import texgen
    o = lin.shape[0] // 8
    out = texgen.quilt_x(lin, o)
    out = texgen.quilt_x(out.transpose(1, 0, 2), o).transpose(1, 0, 2)
    return np.clip(out, 0, 1)


def match_mean(lin, target_lin, strength=1.0, keep_chroma=1.0):
    """Scale to the target mean (linear, per channel) keeping the relative variation; keep_chroma < 1
    pulls the per-pixel chroma toward the mean chroma (removes off-hue spots)."""
    mean = lin.reshape(-1, 3).mean(0)
    g = 1 + (target_lin / np.maximum(mean, 1e-5) - 1) * strength
    out = lin * g
    if keep_chroma < 1:
        lum = out @ np.array([0.2126, 0.7152, 0.0722])
        mlum = max(float(lum.mean()), 1e-5)
        mc = out.reshape(-1, 3).mean(0) / mlum
        out = out * keep_chroma + (lum[..., None] * mc) * (1 - keep_chroma)
    return np.clip(out, 0, 1)


def surface_maps(lin, r):
    """nrh (1024: normal xy, roughness, height) and AO (horizon AO from the height), as texgen + surface_pbr."""
    import surface_pbr
    small = resize_lin(lin, 1024)
    lum = small @ np.array([0.2126, 0.7152, 0.0722])
    hgt = lum - blur(lum, 48, wrap=True)
    hgt = blur(hgt, 1, wrap=True)
    sd = hgt.std() + 1e-6
    hgt = np.clip(0.5 + hgt / (sd * 5.0), 0, 1)
    k = r.get("bump", 4.0)
    dx = (np.roll(hgt, -1, 1) - np.roll(hgt, 1, 1)) * 0.5 * k
    dy = (np.roll(hgt, -1, 0) - np.roll(hgt, 1, 0)) * 0.5 * k
    nz = 1 / np.sqrt(dx * dx + dy * dy + 1)
    rough = np.clip(r.get("rough", 0.88) + (0.5 - hgt) * 0.25, 0.05, 1.0)
    nrh = np.stack([-dx * nz * 0.5 + 0.5, dy * nz * 0.5 + 0.5, rough, hgt], 2)
    # AO at 512 (the horizon search is O(n^2 * dirs * radii)), up to 1024
    h512 = np.asarray(Image.fromarray((hgt * 255).astype(np.uint8)).resize((512, 512), Image.BILINEAR)).astype(np.float64) / 255
    ao = surface_pbr.horizon_ao(h512 * r.get("bump", 4.0) * 4.0)
    med = float(np.median(ao))
    ao = np.clip(ao / max(med, 1e-4), 0, 1) ** 1.5
    ao = np.asarray(Image.fromarray((ao * 255).astype(np.uint8)).resize((1024, 1024), Image.BILINEAR)).astype(np.float64) / 255
    return nrh, ao


def macro_map(r, lin, seed):
    """512 px tileable macro variation: RGB = colour multiplier around 0.5 (0.5 = x1, 0..1 = x0.5..x1.5) from
    two periodic fBm fields (hue toward the layer's accent, value), A = macro height. Covers macro.scale_m."""
    m = r.get("macro", {})
    n = 512
    rng = np.random.default_rng(seed)
    a = PH.pfbm(n, 3, rng, 5)
    b = PH.pfbm(n, 6, rng, 4)
    v = (a - 0.5) * 2 * m.get("val", 0.12)
    acc = hex_lin(m.get("accent", r.get("target_mean", "#808080")))
    base = hex_lin(r.get("target_mean", "#808080"))
    tint = acc / np.maximum(base, 1e-4)
    tint = tint / max(float((tint * np.array([0.2126, 0.7152, 0.0722])).sum()), 1e-4)
    w = PH.smooth(0.45, 0.7, b) * m.get("accent_amount", 0.35)
    mult = (1 + v)[..., None] * ((1 - w)[..., None] + w[..., None] * tint[None, None, :])
    rgb = np.clip(0.5 + (mult - 1) * 1.0, 0, 1)
    return np.concatenate([rgb, a[..., None]], 2)


def process_surface(r, raws, placeholder_only):
    size = r.get("size", 2048)
    src = None if placeholder_only else raws.get(0)
    target = hex_lin(r["target_mean"])
    if src is not None:
        h, w = src.shape[:2]
        s = int(min(h, w) * r.get("crop", 0.6))
        src = src[h - s:, (w - s) // 2:(w - s) // 2 + s]   # bottom-centred: the least perspective
        lin = flatten_light(src)
        lin = resize_lin(lin, int(size * 8 / 7))            # - 2 x 1/8 quilting overlap
        lin = make_tileable(lin)
        if lin.shape[0] != size:
            lin = resize_lin(lin, size)
        source = "generated"
    else:
        spec = dict(r.get("placeholder", {}))
        spec.setdefault("base", r["target_mean"])
        spec.setdefault("tile_m", r["tile_m"])
        lin, _ = PH.ground(spec, size, spec.get("seed", 1))
        source = "placeholder"
    lin = match_mean(lin, target, r.get("color_match", 0.9), r.get("keep_chroma", 1.0))
    nrh, ao = surface_maps(lin, r)
    out = os.path.join(OUT_DIRS[r["_group"]], r["name"])
    save_srgb(out + "_albedo.jpg", lin)
    save_lin(out + "_nrh.png", nrh)
    save_lin(out + "_ao.png", ao)
    save_lin(out + "_macro.png", macro_map(r, lin, r.get("macro", {}).get("seed", 7)))
    mean = lin.reshape(-1, 3).mean(0)
    seam = seam_error(lin)
    return {"size": size, "source": source, "mean": lin_hex(mean), "target_mean": r["target_mean"],
            "mean_delta": round(float(np.abs(l2s(mean) - l2s(target)).max()), 4), "seam": round(seam, 4),
            "tile_m": r["tile_m"]}


def seam_error(lin):
    """Tileability: the mean difference across the wrap (last -> first column, last -> first row)
    relative to the 99th percentile of the same statistic over every interior line pair. <= 1: the wrap
    is no more visible than an ordinary line inside the texture (the test's threshold)."""
    worst = 0.0
    for ax in (0, 1):
        a = np.moveaxis(lin, ax, 0)
        inner = np.abs(a[1:] - a[:-1]).reshape(a.shape[0] - 1, -1).mean(1)
        edge = float(np.abs(a[0] - a[-1]).mean())
        worst = max(worst, edge / max(float(np.percentile(inner, 99)), 1e-6))
    return worst


def process_scatter_atlas(r, raws, placeholder_only):
    cols, rows = r["grid"]
    size = r.get("size", 1024)
    cw = size // cols
    alb = np.zeros((rows * cw, cols * cw, 4))
    nrh = np.zeros((rows * cw, cols * cw, 4))
    cells = []
    for i, c in enumerate(r["cells"]):
        src = None if placeholder_only else raws.get(i)
        if src is not None:
            cell = center_fit(key_out(r, src), cw)
            lum = cell[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
            hgt = np.clip(blur(cell[..., 3], 3) * 0.6 + lum * 0.4, 0, 1)
            source = "generated"
        else:
            cell, hgt = PH.scatter_cell(c["placeholder"], cw, c["placeholder"].get("seed", i + 1))
            source = "placeholder"
        tgt = c.get("target_mean")
        if tgt and (cell[..., 3] > 0.5).any():
            m = cell[..., 3] > 0.5
            g = hex_lin(tgt) / np.maximum(cell[..., :3][m].mean(0), 1e-4)
            cell[..., :3] = np.clip(cell[..., :3] * (1 + (g - 1) * r.get("color_match", 0.8)), 0, 1)
        dx = (np.roll(hgt, -1, 1) - np.roll(hgt, 1, 1)) * 0.5 * r.get("bump", 6.0)
        dy = (np.roll(hgt, -1, 0) - np.roll(hgt, 1, 0)) * 0.5 * r.get("bump", 6.0)
        nz = 1 / np.sqrt(dx * dx + dy * dy + 1)
        y, x = divmod(i, cols)
        alb[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw] = cell
        nrh[y * cw:(y + 1) * cw, x * cw:(x + 1) * cw] = np.stack([-dx * nz * 0.5 + 0.5, dy * nz * 0.5 + 0.5,
                                                                np.full_like(hgt, c.get("rough", 0.8)), hgt], 2)
        cells.append({"source": source, "coverage": round(float((cell[..., 3] > 0.5).mean()), 3), "type": c["placeholder"].get("type")})
    alb = bleed(alb)
    out = os.path.join(OUT_DIRS[r["_group"]], r["name"])
    save_srgb(out + "_albedo.png", alb[..., :3], alb[..., 3])
    save_lin(out + "_nrh.png", nrh)
    return {"grid": [cols, rows], "size": size, "cells": cells}


def process(r, placeholder_only):
    raws = {}
    for i in range(len(cells_of(r))):
        v = load_raw(r, i)
        if v is not None:
            raws[i] = v
    if r["kind"] == "blade_atlas":
        meta = process_blade_atlas(r, raws, placeholder_only)
    elif r["kind"] == "scatter_atlas":
        meta = process_scatter_atlas(r, raws, placeholder_only)
    else:
        meta = process_surface(r, raws, placeholder_only)
    meta.update({"name": r["name"], "kind": r["kind"], "tool_version": TOOL_VERSION, "recipe": os.path.relpath(r["_path"], ROOT).replace("\\", "/")})
    for k in ("tile_m", "pair", "replaces", "layer", "variant_of", "card", "plant"):
        if k in r:
            meta[k] = r[k]
    with open(os.path.join(OUT_DIRS[r["_group"]], r["name"] + ".meta.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(meta, f, indent=1)
        f.write("\n")
    return meta


# ------------------------------------------------------------------ report

def sheet(group, recipes):
    tiles = []
    for name, r in recipes.items():
        if r["_group"] != group:
            continue
        d = OUT_DIRS[group]
        p = os.path.join(d, name + "_albedo." + ("png" if r["kind"] in ("blade_atlas", "scatter_atlas") else "jpg"))
        if not os.path.exists(p):
            continue
        im = Image.open(p).convert("RGBA").resize((256, 256), Image.LANCZOS)
        bg = Image.new("RGBA", im.size, (70, 70, 76, 255))
        bg.alpha_composite(im)
        tiles.append(bg.convert("RGB"))
    if not tiles:
        return
    cols = 8
    rows = (len(tiles) + cols - 1) // cols
    out = Image.new("RGB", (cols * 260, rows * 260), (20, 20, 20))
    for i, t in enumerate(tiles):
        out.paste(t, ((i % cols) * 260 + 2, (i // cols) * 260 + 2))
    os.makedirs(os.path.dirname(SHEET), exist_ok=True)
    out.save(SHEET.format(group))


def write_report(metas, budget):
    lines = ["# Ground texture generation report", "", f"Written by `tools/vfxgen/groundgen.py` (tool v{TOOL_VERSION}). "
             f"Images generated this run: {budget['images']}, spent ${budget['spent']:.2f}.", "",
             "| Recipe | Kind | Source | Checks |", "|---|---|---|---|"]
    for m in metas:
        if "cells" in m:
            src = ", ".join(sorted({c["source"] for c in m["cells"]}))
            fails = sum(len(c.get("fails", [])) for c in m["cells"])
            info = f"{fails} failing cell check(s)" if fails else "ok"
            if m["kind"] == "blade_atlas":
                info += "; root/mid/tip " + " · ".join(f"{c.get('root', '-')}/{c.get('mid', '-')}/{c.get('tip', '-')}" for c in m["cells"])
        else:
            src = m["source"]
            info = f"mean {m['mean']} (target {m['target_mean']}, max delta {m['mean_delta']}), seam {m['seam']}"
        lines.append(f"| {m['name']} | {m['kind']} | {src} | {info} |")
    lines += ["", "Contact sheets: `gen_sheet_foliage.png`, `gen_sheet_terrain.png`.", ""]
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# ------------------------------------------------------------------ git

GIT_PATHS = ["assets/foliage/tex", "assets/terrain/tex", "tools/vfxgen/ground_raw", "docs/ground_match/gen_report.md",
             "docs/ground_match/gen_sheet_foliage.png", "docs/ground_match/gen_sheet_terrain.png"]


def git_commit(push, budget):
    def run(*a):
        return subprocess.run(["git", *a], cwd=ROOT, capture_output=True, text=True)
    paths = [p for p in GIT_PATHS if os.path.exists(os.path.join(ROOT, p))]
    run("add", "--", *paths)   # only the generated outputs; never .env
    if not run("diff", "--cached", "--quiet").returncode:
        print("git: nothing new to commit")
        return
    msg = (f"Ground textures: groundgen run ({budget['images']} images generated, ${budget['spent']:.2f})\n\n"
           "Raw images in tools/vfxgen/ground_raw, processed sets in assets/{foliage,terrain}/tex, report in docs/ground_match.")
    r = run("commit", "-m", msg)
    print(r.stdout.strip() or r.stderr.strip())
    if push:
        br = run("rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
        r = run("push", "-u", "origin", br)
        print(r.stdout.strip() or r.stderr.strip())


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*")
    ap.add_argument("--only", default="")
    ap.add_argument("--regen", default="")
    ap.add_argument("--placeholder", action="store_true")
    ap.add_argument("--process-only", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--max-usd", type=float, default=15.0)
    ap.add_argument("--iterate", type=int, default=2)
    ap.add_argument("--commit", action="store_true")
    ap.add_argument("--push", action="store_true")
    a = ap.parse_args()
    recipes = load_recipes()
    errs = validate(recipes)
    if errs:
        print("\n".join("recipe error: " + e for e in errs))
        return 1
    if a.validate:
        print(f"{len(recipes)} recipes ok ({sum(len(cells_of(r)) for r in recipes.values())} images)")
        return 0
    only = set(filter(None, a.only.split(","))) | set(a.names)
    regen = set(filter(None, a.regen.split(",")))
    todo = {k: v for k, v in recipes.items() if not only or k in only}
    unknown = only - set(recipes)
    if unknown:
        print("unknown recipes: " + ", ".join(sorted(unknown)))
        return 1
    budget = {"spent": 0.0, "cap": a.max_usd, "images": 0}
    if a.dry_run:
        n = 0
        for name, r in todo.items():
            for i in range(len(cells_of(r))):
                have = os.path.exists(raw_path(r, i))
                if not have or name in regen:
                    n += 1
                    print(f"  {name}[{i}]: {gen_params(r, cells_of(r)[i], 0)['prompt'][:110]}...")
        print(f"{n} images to generate, about ${n * 0.19:.2f} at $0.19 (soul/standard 1080p); cap ${a.max_usd:.2f}")
        return 0
    if not (a.placeholder or a.process_only):
        from hf_client import Client
        client = Client(log_name="api_log.jsonl")
        failed = []
        for name, r in todo.items():
            for i in range(len(cells_of(r))):
                if os.path.exists(raw_path(r, i)) and name not in regen:
                    continue
                try:
                    generate_cell(r, i, client, budget, 0, name in regen)
                except Exception as e:   # keep going: the cell falls back to its placeholder
                    print(f"  {name}[{i}]: {e}")
                    failed.append(f"{name}[{i}]")
                    if "cap" in str(e):
                        break
        # checks + re-rolls of failing atlas cells
        for it in range(a.iterate):
            rerolled = 0
            for name, r in todo.items():
                if r["kind"] != "blade_atlas":
                    continue
                meta = process(r, False)
                for i, c in enumerate(meta["cells"]):
                    if c.get("fails"):
                        print(f"  {name}[{i}] fails: {'; '.join(c['fails'])} -> re-roll {it + 1}")
                        try:
                            generate_cell(r, i, client, budget, it + 1, False)
                            rerolled += 1
                        except Exception as e:
                            print(f"  {name}[{i}]: {e}")
            if not rerolled:
                break
        if failed:
            print("not generated (placeholders used): " + ", ".join(failed))
    metas = []
    for name, r in todo.items():
        t0 = time.time()
        m = process(r, a.placeholder)
        metas.append(m)
        print(f"  {name}: {m['kind']} processed in {time.time() - t0:.1f} s")
    for g in GEN_DIRS:
        sheet(g, recipes)
    # the report covers every processed recipe, not only this run's
    all_metas = []
    for name, r in recipes.items():
        mp = os.path.join(OUT_DIRS[r["_group"]], name + ".meta.json")
        if os.path.exists(mp):
            with open(mp, encoding="utf-8") as f:
                all_metas.append(json.load(f))
    write_report(all_metas, budget)
    print(f"report: {os.path.relpath(REPORT, ROOT)}; spent ${budget['spent']:.2f} on {budget['images']} images")
    if a.commit or a.push:
        git_commit(a.push, budget)
    return 0


if __name__ == "__main__":
    sys.exit(main())
