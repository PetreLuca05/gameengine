"""Foliage card textures (docs/TEXTURES.md): grass blades and wheat for the vgeo grass tuft /
crop clump cards (src/render/vgeo/vgeo_procgen_foliage.c: u across the card, image top = card top).

    py tools/vfxgen/foliage_atlas.py

Drawn procedurally (deterministic, no API cost): 4x supersampled polygons, per-blade colour
gradients (dark base, green middle, some dry tips), a lighter midrib, small value noise. Output
512 px RGBA (sRGB colour, linear coverage alpha) in assets/textures/foliage/; empty texels carry the
nearest blade colour so mips and bilinear edges do not darken.
"""
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "textures", "foliage")
SIZE = 512
SS = 4


def lin_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


class Canvas:
    """Linear colour + coverage at SS x supersampling; polygons painted front to back order free
    (later ones on top)."""

    def __init__(self):
        n = SIZE * SS
        self.n = n
        self.col = np.zeros((n, n, 3), np.float32)
        self.cov = np.zeros((n, n), np.float32)

    def poly(self, pts, colour_fn, rows):
        """pts in [0,1] card units (x right, y down); colour_fn(t) by height fraction t (0 base, 1 tip),
        evaluated per pixel row band through `rows` = (y_base, y_tip)."""
        n = self.n
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).polygon([(x * n, y * n) for x, y in pts], fill=255)
        a = np.asarray(m, np.float32) / 255.0
        ys, xs = np.nonzero(a)
        if not len(ys):
            return
        yb, yt = rows[0] * n, rows[1] * n
        t = np.clip((yb - ys) / max(yb - yt, 1e-3), 0, 1)
        c = colour_fn(t, xs / n, ys / n)
        self.col[ys, xs] = c
        self.cov[ys, xs] = 1.0

    def save(self, path):
        n = self.n
        cov = self.cov
        col = self.col
        # bleed colours into empty texels (dilate by repeated max-coverage neighbour fill)
        filled = col.copy()
        have = cov > 0
        for _ in range(24):
            if have.all():
                break
            acc = np.zeros_like(filled)
            cnt = np.zeros(cov.shape, np.float32)
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                sh = np.roll(np.roll(have, dy, 0), dx, 1)
                sc = np.roll(np.roll(filled, dy, 0), dx, 1)
                acc += sc * sh[..., None]
                cnt += sh
            new = (~have) & (cnt > 0)
            filled[new] = acc[new] / cnt[new][:, None]
            have = have | new
        mean = col[cov > 0].mean(axis=0)
        filled[~have] = mean
        # downsample (box)
        c = filled.reshape(SIZE, SS, SIZE, SS, 3).mean(axis=(1, 3))
        a = cov.reshape(SIZE, SS, SIZE, SS).mean(axis=(1, 3))
        cw = (col * cov[..., None]).reshape(SIZE, SS, SIZE, SS, 3).sum(axis=(1, 3))
        cn = cov.reshape(SIZE, SS, SIZE, SS).sum(axis=(1, 3))
        c = np.where(cn[..., None] > 0, cw / np.maximum(cn[..., None], 1e-6), c)   # covered texels: covered samples only
        rgb = (lin_to_srgb(c) * 255 + 0.5).astype(np.uint8)
        alpha = (np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)
        img = np.dstack([rgb, alpha])
        os.makedirs(os.path.dirname(path), exist_ok=True)
        Image.fromarray(img).save(path, optimize=True)


def noise2(seed, cells=24):
    rng = np.random.default_rng(seed)
    g = rng.random((cells, cells)).astype(np.float32)
    im = Image.fromarray((g * 255).astype(np.uint8)).resize((SIZE * SS, SIZE * SS), Image.BICUBIC)
    return np.asarray(im, np.float32) / 255.0


def blade(cv, rnd, x0, height, width, lean, curl, base_c, mid_c, tip_c, nz, rib=True):
    """A tapered, curving blade from (x0, 1) up to height (card fraction)."""
    segs = 14
    left, right = [], []
    for i in range(segs + 1):
        t = i / segs
        y = 1.0 - height * t
        x = x0 + lean * t + curl * t * t
        w = width * (1 - t) ** 0.85 * (0.6 + 0.4 * math.sin(math.pi * min(1, t * 1.6 + 0.25))) + 0.0006
        left.append((x - w, y))
        right.append((x + w, y))
    pts = left + right[::-1]
    def colour(t, xs, ys):
        t = t[:, None]
        c = np.where(t < 0.5, np.array(base_c) + (np.array(mid_c) - np.array(base_c)) * (t / 0.5),
                     np.array(mid_c) + (np.array(tip_c) - np.array(mid_c)) * np.clip((t - 0.5) / 0.5, 0, 1) ** 1.5)
        n = nz[(ys * cv.n).astype(int).clip(0, cv.n - 1), (xs * cv.n).astype(int).clip(0, cv.n - 1)][:, None]
        c = c * (0.82 + 0.36 * n)
        if rib:   # lighter midrib: distance to the blade centre line at that height
            tt = t[:, 0]
            cx = x0 + lean * tt + curl * tt * tt
            wloc = width * (1 - tt) ** 0.85 + 1e-4
            d = np.abs(xs - cx) / wloc
            c = c * (1.0 + 0.25 * np.clip(1 - d * 4, 0, 1))[:, None] * (0.85 + 0.15 * np.clip(d, 0, 1))[:, None]
        return c.astype(np.float32)
    cv.poly(pts, colour, (1.0, 1.0 - height))


def grass():
    rnd = random.Random(7)
    cv = Canvas()
    nz = noise2(3)
    blades = []
    for i in range(28):
        blades.append(dict(x0=0.04 + 0.92 * (i + rnd.random() * 0.8) / 28, height=0.55 + 0.43 * rnd.random(), width=0.008 + 0.009 * rnd.random(),
                           lean=(rnd.random() - 0.5) * 0.22, curl=(rnd.random() - 0.5) * 0.18, dry=rnd.random() < 0.12, depth=rnd.random()))
    blades.sort(key=lambda b: b["depth"])   # back to front
    for b in blades:
        k = 0.8 + 0.2 * b["depth"]           # back blades darker (inner tuft)
        hue = rnd.random()
        base_c = (0.05 * k, 0.09 * k, 0.02 * k)
        mid_c = (lerp((0.12, 0.26, 0.05), (0.17, 0.3, 0.06), hue))
        mid_c = tuple(c * k for c in mid_c)
        tip_c = tuple(c * k for c in lerp((0.2, 0.36, 0.08), (0.38, 0.36, 0.14), 0.35 * hue))
        if b["dry"]:
            mid_c = tuple(c * k for c in (0.26, 0.22, 0.09))
            tip_c = tuple(c * k for c in (0.42, 0.36, 0.18))
        blade(cv, rnd, b["x0"], b["height"], b["width"], b["lean"], b["curl"], base_c, mid_c, tip_c, nz)
    cv.save(os.path.join(OUT, "grass_blades.png"))


def wheat():
    rnd = random.Random(11)
    cv = Canvas()
    nz = noise2(5, 40)
    stalks = [dict(x0=0.05 + 0.9 * (i + rnd.random() * 0.7) / 14, height=0.72 + 0.24 * rnd.random(), lean=(rnd.random() - 0.5) * 0.1, depth=rnd.random())
              for i in range(18)]
    # a few leaves low down
    for i in range(10):
        x0 = 0.05 + 0.9 * rnd.random()
        blade(cv, rnd, x0, 0.25 + 0.25 * rnd.random(), 0.011, (rnd.random() - 0.5) * 0.25, (rnd.random() - 0.5) * 0.3,
              (0.12, 0.1, 0.03), (0.3, 0.26, 0.08), (0.45, 0.37, 0.15), nz, rib=True)
    stalks.sort(key=lambda s: s["depth"])
    for s in stalks:
        k = 0.7 + 0.3 * s["depth"]
        straw = (0.55 * k, 0.45 * k, 0.2 * k)
        ear_top = 1.0 - s["height"]
        ear_len = 0.095 + 0.02 * rnd.random()
        ear_base = ear_top + ear_len
        # stalk
        def cx(t):
            return s["x0"] + s["lean"] * t * t
        top_t = 1.0
        pts_l, pts_r = [], []
        for i in range(12):
            t = i / 11 * ((1.0 - ear_base) / (1.0 - ear_top))
            y = 1.0 - (1.0 - ear_top) * t
            w = 0.0035 * (1 - 0.4 * t)
            pts_l.append((cx(t) - w, y))
            pts_r.append((cx(t) + w, y))
        cv.poly(pts_l + pts_r[::-1], lambda t, xs, ys, c=straw: np.tile(np.array(c, np.float32), (len(xs), 1)) * (0.8 + 0.3 * t[:, None]), (1.0, ear_base))
        # ear: spikelets alternating left / right, awns
        ex = cx((1.0 - ear_base) / (1.0 - ear_top))
        tip_x = cx(1.0)
        nsp = 9
        for j in range(nsp):
            f = (j + 0.5) / nsp
            y = ear_base - ear_len * f
            x = ex + (tip_x - ex) * f
            side = -1 if j % 2 else 1
            rw, rh = 0.0075 * (1 - 0.35 * f), 0.011
            ang = side * 0.45
            pts = []
            for q in range(10):
                a = q / 10 * 2 * math.pi
                px, py = math.cos(a) * rw, math.sin(a) * rh
                pts.append((x + side * rw * 0.6 + px * math.cos(ang) - py * math.sin(ang), y + px * math.sin(ang) + py * math.cos(ang)))
            gold = (0.72 * k, 0.58 * k, 0.27 * k)
            def colour(t, xs, ys, g=gold, x=x, y=y):
                d = np.sqrt((xs - x) ** 2 + (ys - y) ** 2) / 0.012
                n = nz[(ys * cv.n).astype(int).clip(0, cv.n - 1), (xs * cv.n).astype(int).clip(0, cv.n - 1)]
                return (np.array(g, np.float32)[None, :] * (1.15 - 0.45 * np.clip(d, 0, 1))[:, None] * (0.85 + 0.3 * n)[:, None]).astype(np.float32)
            cv.poly(pts, colour, (1.0, 0.0))
            # awn: a thin line up and outwards
            ax, ay = x + side * rw * 1.2, y - rh * 0.6
            bx, by = ax + side * 0.012 + (tip_x - ex) * 0.1, ay - 0.05
            wv = 0.0011
            cv.poly([(ax - wv, ay), (ax + wv, ay), (bx + wv * 0.3, by), (bx - wv * 0.3, by)],
                    lambda t, xs, ys, c=(0.55 * k, 0.45 * k, 0.22 * k): np.tile(np.array(c, np.float32), (len(xs), 1)), (1.0, 0.0))
    cv.save(os.path.join(OUT, "wheat.png"))


if __name__ == "__main__":
    grass()
    wheat()
    print("ok")
