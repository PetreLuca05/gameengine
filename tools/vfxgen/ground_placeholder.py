"""Procedural placeholders for the ground recipes (groundgen.py --placeholder; docs/GROUND_REFERENCE.md).

Free and deterministic. They follow the target spec (blade shape, gradient, dry ratio, ground colours),
so the game looks right in kind before any image is generated; the generated sets replace them.

  blade_cell(spec, size, seed)  -> (H, W, 4) linear RGBA of one atlas cell (blades rising from the bottom)
  ground(spec, size, seed)      -> (H, W, 3) linear tileable albedo + (H, W) height
  scatter_cell(spec, size, seed) -> (H, W, 4) linear RGBA + (H, W) height of one scatter sprite
"""
import math

import numpy as np


def hex_lin(h):
    h = h.lstrip("#")
    c = np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# ------------------------------------------------------------------ periodic noise (tileable)

def pnoise(n, cells, rng):
    """Periodic value noise on an n x n grid with `cells` lattice cells per side (smoothstep)."""
    g = rng.random((cells, cells))
    x = np.arange(n) * cells / n
    i0 = np.floor(x).astype(int)
    f = x - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    a = g[i0][:, i0] * (1 - f)[None, :] + g[i0][:, i1] * f[None, :]
    b = g[i1][:, i0] * (1 - f)[None, :] + g[i1][:, i1] * f[None, :]
    return a * (1 - f)[:, None] + b * f[:, None]


def pfbm(n, cells, rng, octaves=5, gain=0.5):
    out = np.zeros((n, n))
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        c = cells * (2 ** o)
        if c > n:
            break
        out += pnoise(n, c, rng) * amp
        tot += amp
        amp *= gain
    return out / tot


def stamp_ellipse(img, hgt, cx, cy, rx, ry, ang, col, h, n, soft=1.0):
    """Wrap-around ellipse stamp (colour + dome height) on n x n arrays."""
    r = int(max(rx, ry)) + 2
    ys, xs = np.mgrid[-r:r + 1, -r:r + 1]
    c, s = math.cos(ang), math.sin(ang)
    u = (xs * c + ys * s) / max(rx, 0.5)
    v = (-xs * s + ys * c) / max(ry, 0.5)
    d = u * u + v * v
    m = np.clip((1 - d) * 3 * soft, 0, 1)
    if not m.any():
        return
    yy = (int(cy) + ys) % n
    xx = (int(cx) + xs) % n
    dome = np.sqrt(np.clip(1 - d, 0, 1)) * h
    for k in range(3):
        img[yy, xx, k] = img[yy, xx, k] * (1 - m) + col[k] * m
    hgt[yy, xx] = np.maximum(hgt[yy, xx], hgt[yy, xx] * (1 - m) + dome * m + 0.0)


def stamp_line(img, hgt, x0, y0, x1, y1, w, col, h, n):
    L = max(1, int(math.hypot(x1 - x0, y1 - y0)))
    for t in np.linspace(0, 1, L * 2):
        stamp_ellipse(img, hgt, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w, w, 0, col, h, n)


# ------------------------------------------------------------------ ground layers

GROUND_TYPES = ("litter", "moss", "dirt", "dirt_dry", "pale", "path", "field", "mud", "sand", "rock", "gravel",
                "needles", "pebble_band", "sparse_grass", "wet_edge")


def ground(spec, size, seed):
    """Tileable ground placeholder. spec: type, base (hex), accent (hex list), detail density keys."""
    rng = np.random.default_rng(seed)
    n = size
    t = spec.get("type", "dirt")
    base = hex_lin(spec.get("base", "#5e5540"))
    acc = [hex_lin(a) for a in spec.get("accent", [])] or [base * 0.7, base * 1.3]
    lo = pfbm(n, 4, rng, 6)
    hi = pfbm(n, 32, rng, 3)
    var = (lo - 0.5) * spec.get("macro_var", 0.35) + (hi - 0.5) * spec.get("fine_var", 0.25)
    img = base[None, None, :] * (1 + var)[..., None]
    hgt = 0.35 + 0.3 * hi + 0.15 * lo
    # mottling with the first accent
    mot = smooth(0.55, 0.75, pfbm(n, 8, rng, 4))
    img = img * (1 - mot[..., None] * 0.6) + acc[0][None, None, :] * (mot[..., None] * 0.6) * (1 + var[..., None] * 0.5)
    px_m = n / spec.get("tile_m", 3.0)   # pixels per metre
    count = lambda per_m2: int(per_m2 * (n / px_m) ** 2)
    if t in ("litter", "needles", "sparse_grass", "moss", "field"):
        # small leaves (ellipses), twigs, grass bits
        leaf_cols = acc[1:] or [base * 1.4]
        for _ in range(count(spec.get("leaves", 120))):
            col = leaf_cols[rng.integers(len(leaf_cols))] * rng.uniform(0.7, 1.25)
            r = rng.uniform(0.006, 0.018) * px_m
            stamp_ellipse(img, hgt, rng.uniform(0, n), rng.uniform(0, n), r, r * rng.uniform(0.35, 0.6), rng.uniform(0, 6.3), col, 0.75, n)
        for _ in range(count(spec.get("twigs", 6))):
            x, y = rng.uniform(0, n, 2)
            a = rng.uniform(0, 6.3)
            L = rng.uniform(0.04, 0.15) * px_m
            stamp_line(img, hgt, x, y, x + math.cos(a) * L, y + math.sin(a) * L, max(0.6, 0.003 * px_m), base * 1.5, 0.8, n)
        for _ in range(count(spec.get("blades", 0))):
            x, y = rng.uniform(0, n, 2)
            a = rng.uniform(0, 6.3)
            L = rng.uniform(0.03, 0.09) * px_m
            col = (leaf_cols[-1] if rng.random() < 0.5 else acc[0]) * rng.uniform(0.8, 1.3)
            stamp_line(img, hgt, x, y, x + math.cos(a) * L, y + math.sin(a) * L, max(0.5, 0.0025 * px_m), col, 0.7, n)
    if t in ("dirt", "dirt_dry", "pale", "path", "pebble_band", "gravel", "sparse_grass", "wet_edge", "mud", "field"):
        # clods / pebbles
        for _ in range(count(spec.get("pebbles", 30))):
            col = (acc[-1] if rng.random() < 0.35 else base * rng.uniform(0.75, 1.5))
            r = rng.uniform(0.004, spec.get("pebble_max", 0.02)) * px_m
            stamp_ellipse(img, hgt, rng.uniform(0, n), rng.uniform(0, n), r, r * rng.uniform(0.6, 1.0), rng.uniform(0, 6.3), col, 1.0, n)
        cracks = spec.get("cracks", 0)
        if cracks:
            cr = np.abs(pfbm(n, 6, rng, 4) - 0.5)
            m = smooth(0.02 * cracks, 0.0, cr)
            img *= (1 - 0.45 * m)[..., None]
            hgt -= 0.25 * m
    if t == "rock":
        ridge = 1 - np.abs(pfbm(n, 3, rng, 6) * 2 - 1)
        hgt = 0.2 + 0.6 * ridge + 0.2 * hi
        img *= (0.7 + 0.5 * ridge)[..., None]
    if t == "sand":
        y = np.arange(n)[:, None] / n
        rip = np.sin((y * 40 + pfbm(n, 4, rng, 3) * 6) * 2 * math.pi) * 0.5 + 0.5
        hgt = 0.4 + 0.2 * rip + 0.1 * hi
        img *= (0.92 + 0.12 * rip)[..., None]
    if t == "mud" or t == "wet_edge":
        wet = smooth(0.5, 0.7, lo)
        img *= (1 - 0.35 * wet)[..., None]
    # cavities darker (a cheap baked AO so the placeholder is not flat)
    cav = hgt - (pfbm(n, 16, rng, 2) * 0 + hgt.mean())
    img *= (0.9 + 0.2 * np.clip(cav + 0.5, 0, 1))[..., None]
    # normalise to the target mean (linear) keeping the variation
    img = img * (base / np.maximum(img.reshape(-1, 3).mean(0), 1e-5))[None, None, :]
    return np.clip(img, 0, 1), np.clip(hgt, 0, 1)


# ------------------------------------------------------------------ blades

def _blade(cv, a_buf, sid, x0, y0, H, W, lean, arch, droop_side, cols, rib, nz, rng, ss):
    """One blade from (x0, y0) (bottom) upward: straight lower part, arching over at the top; tapers
    from 65 % of its length. cols = (root, mid, tip) linear."""
    n = cv.shape[0]
    steps = int(H * 2.2) + 8
    px, py = x0, y0
    ang = -math.pi / 2 + lean
    seg = H / steps
    for i in range(steps):
        t = i / (steps - 1)
        # curvature grows with t (arch over)
        ang += droop_side * arch * 4.2 * (t ** 2) / steps
        px += math.cos(ang) * seg
        py += math.sin(ang) * seg
        w = W * (1.0 if t < 0.65 else max(0.0, 1 - (t - 0.65) / 0.35) ** 0.8)
        w = max(w, 0.35 * ss)
        if t < 0.04:
            w *= 0.8 + 5 * t
        c = cols[0] * (1 - smooth(0, 0.5, t)) + cols[1] * smooth(0, 0.5, t) * (1 - smooth(0.5, 1, t)) + cols[2] * smooth(0.5, 1, t)
        r = int(w) + 2
        ix, iy = int(px), int(py)
        if iy < -r or iy >= n + r or ix < -r or ix >= n + r:
            continue
        ys, xs = np.mgrid[max(0, iy - r):min(n, iy + r + 1), max(0, ix - r):min(n, ix + r + 1)]
        if xs.size == 0:
            continue
        # distance across the blade direction
        nx_, ny_ = -math.sin(ang), math.cos(ang)
        across = (xs - px) * nx_ + (ys - py) * ny_
        along = (xs - px) * math.cos(ang) + (ys - py) * math.sin(ang)
        d = np.hypot(across, along) / max(w, 1e-3)   # overlapping round stamps (step < 1 px): no gaps
        m = np.clip((1 - d) * w * 0.9, 0, 1)
        if not m.any():
            continue
        shade = 1 + rib * smooth(0.25, 0.0, np.abs(across) / max(w, 1e-3)) - 0.10 * (across / max(w, 1e-3) > 0.2)
        shade = shade * (1 + (nz[np.clip(ys, 0, n - 1), np.clip(xs, 0, n - 1)] - 0.5) * 0.08)
        old = a_buf[ys, xs]
        k = m * (1 - 0.0 * old)
        for ch in range(3):
            cv[ys, xs, ch] = cv[ys, xs, ch] * (1 - k) + c[ch] * shade * k
        a_buf[ys, xs] = np.maximum(old, m)
        sid[ys, xs] = np.where(m > 0.5, t, sid[ys, xs])


def blade_cell(spec, size, seed):
    """One card cell: blades rising from the bottom edge. spec keys (from the recipe's placeholder):
    blades [lo, hi], height [lo, hi] (fraction of the cell), width (fraction of the cell), arch, fan
    (degrees), dry (ratio), rib, plume (ratio of stems with a seed plume), flowers (ratio), flower_cols."""
    ss = 2
    n = size * ss
    rng = np.random.default_rng(seed)
    cv = np.zeros((n, n, 3))
    a = np.zeros((n, n))
    sid = np.zeros((n, n))
    t = spec.get("target", {})
    g = [hex_lin(t.get(k, d)) for k, d in (("root", "#4a5030"), ("mid", "#5c6435"), ("tip", "#6f7340"))]
    dr = [hex_lin(t.get(k, d)) for k, d in (("dry_root", "#7d7650"), ("dry_mid", "#948660"), ("dry_tip", "#a8986c"))]
    nz = pfbm(n, 24, rng, 3)
    lo, hi = spec.get("blades", [6, 9])
    count = int(rng.integers(lo, hi + 1))
    hlo, hhi = spec.get("height", [0.7, 0.95])
    W = spec.get("width", 0.02) * n / 2
    fan = math.radians(spec.get("fan", 18))
    arch = spec.get("arch", 0.35)
    order = rng.permutation(count)
    for k in range(count):
        back = order[k] / max(1, count - 1)   # earlier = further back = darker
        dry = rng.random() < spec.get("dry", 0.1)
        cols = dr if dry else g
        dim = 0.78 + 0.22 * back
        cols = [c * dim * rng.uniform(0.92, 1.08) for c in cols]
        x0 = n * (0.5 + rng.uniform(-0.28, 0.28) * spec.get("spread", 1.0))
        lean = rng.uniform(-fan, fan)
        side = 1 if lean > 0 else -1
        if rng.random() < 0.25:
            side = -side
        H = n * rng.uniform(hlo, hhi)
        _blade(cv, a, sid, x0, n - 1, H, W * rng.uniform(0.75, 1.2), lean, arch * rng.uniform(0.5, 1.4), side, cols,
               spec.get("rib", 0.06), nz, rng, ss)
        if rng.random() < spec.get("plume", 0):
            # feathery plume: a cloud of small pale ellipses near the stem top
            tx = x0 + math.sin(lean) * H * 0.95
            ty = n - 1 - math.cos(lean) * H * 0.95
            pc = hex_lin(spec.get("plume_col", "#c8c4b0"))
            for _ in range(160):
                u = rng.uniform(0, 1)
                px_ = tx + math.sin(lean) * u * H * 0.14 + rng.normal(0, W * 1.5)
                py_ = ty - math.cos(lean) * u * H * 0.14 + rng.normal(0, W * 0.8)
                _dot(cv, a, px_, py_, W * 0.45, pc * rng.uniform(0.85, 1.1))
        if rng.random() < spec.get("flowers", 0):
            fcols = spec.get("flower_cols", ["#c46a9a", "#8a6ac0", "#e0c040"])
            fc = hex_lin(fcols[rng.integers(len(fcols))])
            tx = x0 + math.sin(lean) * H * 0.8
            ty = n - 1 - math.cos(lean) * H * 0.8
            for _ in range(rng.integers(3, 7)):
                _dot(cv, a, tx + rng.normal(0, W * 2), ty + rng.normal(0, W * 2), W * rng.uniform(1.0, 1.8), fc * rng.uniform(0.8, 1.15))
    # down to the cell size (box filter = 2x supersampling)
    out = np.concatenate([cv, a[..., None]], axis=2)
    out = out.reshape(size, ss, size, ss, 4).mean(axis=(1, 3))
    rgb = out[..., :3] / np.maximum(out[..., 3:], 1e-4)
    return np.concatenate([np.clip(rgb, 0, 1), out[..., 3:]], axis=2)


def _dot(cv, a, x, y, r, col):
    n = cv.shape[0]
    ri = int(r) + 2
    ix, iy = int(x), int(y)
    if iy < -ri or iy >= n + ri or ix < -ri or ix >= n + ri:
        return
    ys, xs = np.mgrid[max(0, iy - ri):min(n, iy + ri + 1), max(0, ix - ri):min(n, ix + ri + 1)]
    if xs.size == 0:
        return
    d = np.hypot(xs - x, ys - y) / max(r, 0.5)
    m = np.clip((1 - d) * r, 0, 1)
    for ch in range(3):
        cv[ys, xs, ch] = cv[ys, xs, ch] * (1 - m) + col[ch] * m
    a[ys, xs] = np.maximum(a[ys, xs], m)


# ------------------------------------------------------------------ scatter sprites

def scatter_cell(spec, size, seed):
    """One scatter sprite (pebble, leaf, twig, grass bits, crack) centred in the cell."""
    rng = np.random.default_rng(seed)
    n = size
    img = np.zeros((n, n, 3))
    hgt = np.zeros((n, n))
    t = spec.get("type", "pebble")
    col = hex_lin(spec.get("color", "#8a8270"))
    if t == "pebble":
        for _ in range(rng.integers(1, 4)):
            r = n * rng.uniform(0.12, 0.28)
            stamp_ellipse(img, hgt, n / 2 + rng.normal(0, n * 0.1), n / 2 + rng.normal(0, n * 0.1), r, r * rng.uniform(0.6, 0.95),
                          rng.uniform(0, 6.3), col * rng.uniform(0.7, 1.3), 1.0, n)
    elif t == "leaf":
        r = n * 0.36
        stamp_ellipse(img, hgt, n / 2, n / 2, r, r * 0.42, rng.uniform(0, 6.3), col * rng.uniform(0.8, 1.2), 0.35, n)
    elif t in ("twig", "grass_bits"):
        k = 1 if t == "twig" else 6
        for _ in range(k):
            a = rng.uniform(0, 6.3)
            L = n * (0.42 if t == "twig" else rng.uniform(0.15, 0.35))
            stamp_line(img, hgt, n / 2 - math.cos(a) * L / 2, n / 2 - math.sin(a) * L / 2, n / 2 + math.cos(a) * L / 2,
                       n / 2 + math.sin(a) * L / 2, n * (0.018 if t == "twig" else 0.008), col * rng.uniform(0.8, 1.2), 0.6, n)
    elif t == "crack":
        x, y = n * 0.2, n * 0.5
        for _ in range(12):
            nx_, ny_ = x + n * 0.06, y + rng.normal(0, n * 0.05)
            stamp_line(img, hgt, x, y, nx_, ny_, n * 0.008, col, -0.3, n)
            x, y = nx_, ny_
    alpha = (np.abs(hgt) > 1e-3).astype(float)
    ys, xs = np.mgrid[0:n, 0:n]
    alpha *= smooth(0.5, 0.42, np.maximum(np.abs(xs / n - 0.5), np.abs(ys / n - 0.5)))
    return np.concatenate([np.clip(img, 0, 1), alpha[..., None]], axis=2), np.clip(hgt * 0.5 + 0.5, 0, 1)
