"""Procedural placeholder textures (vfxgen --placeholder): deterministic per recipe seed, no API.
Every generator returns a list of straight-alpha RGBA float frames (cell x cell); vfxgen then runs
the same post-processing as for generated sources (premultiply, flow, 6-way, normals, packing).
"""
import math

import numpy as np

# ------------------------------------------------------------------ noise


def _hash(ix, iy, iz, seed):
    h = (ix * 374761393 + iy * 668265263 + iz * 2147483647 + seed * 144269504) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF).astype(np.float32) / 16777215.0


def value_noise3(x, y, z, seed=0, period=None):
    xi, yi, zi = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64), np.floor(z).astype(np.int64)
    fx, fy, fz = x - xi, y - yi, z - zi
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    fz = fz * fz * (3 - 2 * fz)
    out = 0
    for dz in (0, 1):
        for dy in (0, 1):
            for dx in (0, 1):
                ax, ay = xi + dx, yi + dy
                if period:
                    ax, ay = ax % period, ay % period
                w = (fx if dx else 1 - fx) * (fy if dy else 1 - fy) * (fz if dz else 1 - fz)
                out = out + w * _hash(ax, ay, zi + dz, seed)
    return out


def fbm(x, y, z=0.0, octaves=5, seed=0, lacunarity=2.0, gain=0.5):
    amp, freq, total, norm = 1.0, 1.0, 0.0, 0.0
    z = np.broadcast_to(np.asarray(z, dtype=np.float32), np.shape(x))
    for o in range(octaves):
        total = total + amp * value_noise3(x * freq, y * freq, z * freq, seed + o * 17)
        norm += amp
        amp *= gain
        freq *= lacunarity
    return total / norm


def grid(n):
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float32)
    u = (xs + 0.5) / n * 2 - 1
    v = (ys + 0.5) / n * 2 - 1
    return u, v


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def rgba(col, a):
    col = np.asarray(col, dtype=np.float32)
    if col.ndim == 1:
        col = np.broadcast_to(col, a.shape + (3,))
    return np.concatenate([np.clip(col, 0, 1), np.clip(a, 0, 1)[..., None]], axis=2).astype(np.float32)


def heat_ramp(t):
    """Black-body-ish ramp: white core -> yellow -> orange -> dark red."""
    t = np.clip(t, 0, 1)[..., None]
    c0 = np.array([0.45, 0.06, 0.01], np.float32)
    c1 = np.array([1.0, 0.42, 0.06], np.float32)
    c2 = np.array([1.0, 0.82, 0.38], np.float32)
    c3 = np.array([1.0, 0.98, 0.9], np.float32)
    return np.where(t < 0.33, c0 + (c1 - c0) * (t / 0.33),
                    np.where(t < 0.7, c1 + (c2 - c1) * ((t - 0.33) / 0.37), c2 + (c3 - c2) * ((t - 0.7) / 0.3)))


# ------------------------------------------------------------------ emissive


def flash_core(cell, variants, seed, spikes=(5, 8), **_):
    """Front view muzzle flash: hot core + irregular star of flame petals."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)
    for i in range(variants):
        n = int(rng.integers(spikes[0], spikes[1] + 1))
        phase = rng.uniform(0, 2 * math.pi)
        lens = rng.uniform(0.45, 1.0, n)
        widths = rng.uniform(0.18, 0.42, n)
        petal = np.zeros_like(r)
        for k in range(n):
            a0 = phase + k * 2 * math.pi / n + rng.uniform(-0.25, 0.25)
            d = np.angle(np.exp(1j * (ang - a0)))
            w = widths[k] * (1.0 - r / max(lens[k], 1e-3)).clip(0, 1)
            petal = np.maximum(petal, np.exp(-(d * d) / (2 * (w * 0.45 + 1e-3) ** 2)) * smooth(lens[k], lens[k] * 0.2, r))
        noise = fbm(u * 3.5 + i * 7.1, v * 3.5, 0.0, 4, seed + i)
        core = np.exp(-(r * r) / 0.035)
        heat = np.clip(core * 1.1 + petal * (0.55 + 0.6 * noise) - r * 0.15, 0, 1)
        a = np.clip(heat * 1.4, 0, 1) * smooth(1.0, 0.85, r)
        out.append(rgba(heat_ramp(heat), a))
    return out


def flash_side(cell, variants, seed, **_):
    """Side flame tongue: base at the bottom of the image, tip at the top (axis quads)."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    t = (1 - v) * 0.5          # 0 at the bottom (muzzle) .. 1 at the top (tip)
    for i in range(variants):
        length = rng.uniform(0.7, 1.0)
        bulge = rng.uniform(0.25, 0.45)
        width = bulge * np.sin(np.clip(t / length, 0, 1) * math.pi) ** 0.7 * (1 - 0.3 * t)
        n = fbm(u * 4 + i * 3.3, t * 6 - i, 0.0, 4, seed + i * 5)
        edge = np.abs(u) / np.maximum(width + 0.12 * (n - 0.5), 1e-3)
        body = smooth(1.0, 0.35, edge) * smooth(length, length * 0.55, t) * smooth(0.0, 0.06, t)
        heat = np.clip(body * (0.7 + 0.5 * n) * (1.15 - t * 0.6), 0, 1)
        out.append(rgba(heat_ramp(heat), np.clip(heat * 1.5, 0, 1)))
    return out


def flash_suppressed(cell, variants, seed, **_):
    out = []
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    for i in range(variants):
        n = fbm(u * 3 + i, v * 3, 0.0, 4, seed + i)
        heat = np.clip(np.exp(-(r * r) / 0.06) * (0.5 + 0.5 * n), 0, 1) * 0.6
        out.append(rgba(heat_ramp(heat * 0.8), heat))
    return out


def spark(cell, variants=1, seed=0, **_):
    """Glow streak along the image vertical (stretched particles): hot head at the top."""
    u, v = grid(cell)
    t = (1 - v) * 0.5
    core = np.exp(-(u * u) / 0.004) * smooth(0.0, 0.25, t) * smooth(1.0, 0.8, t)
    glow = np.exp(-(u * u) / 0.04) * smooth(0.0, 0.5, t) * smooth(1.0, 0.7, t) * 0.35
    heat = np.clip(core + glow, 0, 1)
    return [rgba(heat_ramp(0.55 + 0.45 * heat), heat)]


def ember(cell, variants=1, seed=0, **_):
    u, v = grid(cell)
    r2 = u * u + v * v
    heat = np.clip(np.exp(-r2 / 0.03) + np.exp(-r2 / 0.25) * 0.25, 0, 1)
    return [rgba(heat_ramp(0.4 + 0.6 * heat), heat)]


def glow(cell, variants=1, seed=0, **_):
    u, v = grid(cell)
    r2 = u * u + v * v
    a = np.clip(np.exp(-r2 / 0.18), 0, 1) * smooth(1.0, 0.8, np.sqrt(r2))
    return [rgba([1, 1, 1], a)]


# ------------------------------------------------------------------ volumes


def puff(cell, frames, seed, grow=(0.35, 1.0), density=1.0, detail=4.0, rise=0.15, swirl=0.6, fade=(0.0, 1.0), loop=False, **_):
    """Expanding billowing puff (smoke / dust / mist): density in alpha, albedo white
    (tinted per emitter; lit by the 6-way maps)."""
    out = []
    u, v = grid(cell)
    for f in range(frames):
        t = f / max(frames - 1, 1)
        radius = grow[0] + (grow[1] - grow[0]) * (1 - (1 - t) ** 2)
        cy = -rise * t
        du, dv = u / radius, (v - cy) / radius
        r = np.sqrt(du * du + dv * dv)
        n = fbm(du * detail * 0.5 + 11.3, dv * detail * 0.5 - t * swirl, t * 1.5, 5, seed)
        n2 = fbm(du * detail + 3.1 + t * swirl, dv * detail, t * 2.5 + 4, 4, seed + 9)
        shape = 1.0 - r + (n - 0.5) * 0.9 + (n2 - 0.5) * 0.35
        d = smooth(0.0, 0.45, shape) * density
        d *= (1 - smooth(fade[0], fade[1], t) * 0.85) if not loop else 1
        d *= smooth(1.0, 0.82, np.sqrt(u * u + v * v))
        out.append(rgba([1, 1, 1], np.clip(d, 0, 1)))
    return out


def mist(cell, frames, seed, **kw):
    kw.setdefault("grow", (0.25, 0.9))
    kw.setdefault("density", 0.8)
    kw.setdefault("detail", 6.0)
    kw.setdefault("rise", 0.0)
    return puff(cell, frames, seed, **kw)


def splash(cell, frames, seed, drops=26, **_):
    """Water splash: crown of droplets rising and falling plus a sheet."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    ang = rng.uniform(-1.1, 1.1, drops)
    spd = rng.uniform(0.6, 1.3, drops)
    size = rng.uniform(0.015, 0.05, drops)
    for f in range(frames):
        t = f / max(frames - 1, 1)
        a = np.zeros_like(u)
        for k in range(drops):
            x = math.sin(ang[k]) * spd[k] * t * 0.9
            y = 0.85 - (math.cos(ang[k]) * spd[k] * 2.2 * t - 2.4 * t * t) * 0.9
            s = size[k] * (1 - 0.5 * t)
            a = np.maximum(a, np.exp(-((u - x) ** 2 + (v - y) ** 2) / (2 * s * s)))
        sheet_h = 0.9 * math.sin(min(t * 1.6, 1) * math.pi) * (1 - t)
        sheet = smooth(0.5, 0.2, np.abs(u) / max(0.25 + t * 0.5, 1e-3)) * smooth(0.85 - sheet_h - 0.05, 0.85 - sheet_h * 0.4, v) * smooth(0.95, 0.85, v)
        n = fbm(u * 6, v * 6, t * 3, 3, seed)
        a = np.clip(np.maximum(a, sheet * (0.4 + 0.6 * n)) * (1 - t * 0.6), 0, 1)
        out.append(rgba([0.85, 0.92, 1.0], a))
    return out


def ripple(cell, variants=1, seed=0, **_):
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ring = np.exp(-((r - 0.7) ** 2) / 0.004) + 0.4 * np.exp(-((r - 0.45) ** 2) / 0.003)
    return [rgba([0.9, 0.95, 1.0], np.clip(ring, 0, 1) * smooth(1.0, 0.9, r))]


# ------------------------------------------------------------------ debris sprites


def _polygon_mask(u, v, pts):
    """Inside test for a convex-ish polygon (points in uv)."""
    inside = np.ones_like(u, dtype=bool)
    n = len(pts)
    sign = None
    for i in range(n):
        x0, y0 = pts[i]
        x1, y1 = pts[(i + 1) % n]
        c = (x1 - x0) * (v - y0) - (y1 - y0) * (u - x0)
        s = c >= 0
        if sign is None:
            sign = s
        inside &= (s == sign)
    return inside


def chunks(cell, variants, seed, color=(0.6, 0.6, 0.58), elong=1.0, roughness=0.35, translucent=0.0, **_):
    """Irregular chunks (concrete, brick, wood splinters, glass shards): alpha cutout with a simple
    baked top-left lighting so they read as solids even unlit."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    for i in range(variants):
        n = int(rng.integers(5, 9))
        angs = np.sort(rng.uniform(0, 2 * math.pi, n))
        rad = rng.uniform(0.45, 0.85, n)
        pts = [(math.cos(a) * r * (1.0 / elong if elong > 1 else 1), math.sin(a) * r * (elong if elong > 1 else 1)) for a, r in zip(angs, rad)]
        rot = rng.uniform(0, math.pi)
        cr, sr = math.cos(rot), math.sin(rot)
        ru, rv = u * cr - v * sr, u * sr + v * cr
        m = _polygon_mask(ru, rv, pts).astype(np.float32)
        nz = fbm(u * 8 + i, v * 8, 0, 4, seed + i)
        h = m * (0.6 + roughness * (nz - 0.5))
        gx = np.gradient(h, axis=1)
        gy = np.gradient(h, axis=0)
        shade = np.clip(0.75 - (gx - gy) * 6.0, 0.35, 1.15)
        col = np.asarray(color, np.float32)[None, None, :] * shade[..., None] * (0.85 + 0.3 * nz[..., None])
        a = m * (1 - translucent) + m * translucent * (0.35 + 0.65 * np.clip(np.abs(gx - gy) * 20, 0, 1))
        out.append(rgba(col, a))
    return out


def drops(cell, variants, seed, color=(0.45, 0.02, 0.02), **_):
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    for i in range(variants):
        a = np.zeros_like(u)
        for k in range(int(rng.integers(1, 4))):
            x, y = rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3)
            s = rng.uniform(0.12, 0.3)
            a = np.maximum(a, smooth(s, s * 0.6, np.sqrt((u - x) ** 2 + (v - y) ** 2 * 0.6)))
        out.append(rgba(color, a))
    return out


def grass_bits(cell, variants, seed, **_):
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    for i in range(variants):
        a = np.zeros_like(u)
        for k in range(int(rng.integers(2, 5))):
            ang = rng.uniform(0, math.pi)
            c, s = math.cos(ang), math.sin(ang)
            pu, pv = u * c - v * s, u * s + v * c
            a = np.maximum(a, smooth(0.08, 0.02, np.abs(pv) + np.abs(pu) * 0.08) * smooth(0.9, 0.7, np.abs(pu)))
        col = np.stack([0.25 + 0.1 * u, 0.42 + 0.1 * v, 0.12 + 0 * u], axis=2)
        out.append(rgba(col, a))
    return out


# ------------------------------------------------------------------ distortion


def heat_normal(cell, variants=1, seed=0, **_):
    """Distortion sprite: rg = refraction direction (normal map), a = strength (radial falloff)."""
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    n = fbm(u * 3, v * 3, 0, 4, seed)
    gx = np.gradient(n, axis=1) * cell * 0.25
    gy = np.gradient(n, axis=0) * cell * 0.25
    a = smooth(1.0, 0.3, r)
    return [np.stack([np.clip(0.5 + gx, 0, 1), np.clip(0.5 - gy, 0, 1), np.full_like(u, 1.0), a], axis=2).astype(np.float32)]


# ------------------------------------------------------------------ decals (albedo + height for the normal map)


def decal_hole(cell, variants, seed, kind="concrete", **_):
    """Bullet hole decals. Returns frames with a height map packed for the normal pass:
    the generator stores height in frame['height'] (list parallel to the frames)."""
    out, heights = [], []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)
    for i in range(variants):
        n = fbm(u * 6 + i * 2.7, v * 6, 0, 5, seed + i)
        jag = 1 + 0.25 * (fbm(np.cos(ang) * 2 + i, np.sin(ang) * 2, 0, 3, seed + 31 + i) - 0.5)
        rr = r * jag
        if kind == "metal":
            hole = smooth(0.16, 0.12, rr)
            rim = np.exp(-((rr - 0.2) ** 2) / 0.004)
            dent = smooth(0.55, 0.15, rr)
            col = np.stack([0.55 + 0.3 * rim, 0.55 + 0.3 * rim, 0.58 + 0.3 * rim], 2) * (1 - hole[..., None] * 0.9)
            a = np.clip(hole + rim * 0.8 + dent * 0.35 * (0.5 + n * 0.5), 0, 1) * smooth(0.9, 0.6, r)
            h = dent * -0.6 + rim * 0.5 - hole
        elif kind == "wood":
            hole = smooth(0.17, 0.1, rr)
            splint = smooth(0.7, 0.1, rr) * (fbm(u * 2 + i, v * 14, 0, 4, seed + 7 + i) > 0.55)
            col = np.stack([0.32 + 0 * u, 0.2 + 0 * u, 0.1 + 0 * u], 2) * (1 - hole[..., None] * 0.85) + 0.25 * splint[..., None] * np.array([0.9, 0.75, 0.5])
            a = np.clip(hole + splint * 0.8 + smooth(0.45, 0.15, rr) * 0.5, 0, 1) * smooth(0.9, 0.6, r)
            h = -hole + splint * 0.4
        elif kind == "glass":
            hole = smooth(0.1, 0.06, rr)
            cracks = np.zeros_like(u)
            for k in range(int(rng.integers(6, 11))):
                a0 = rng.uniform(0, 2 * math.pi)
                d = np.abs(np.angle(np.exp(1j * (ang - a0))))
                cracks = np.maximum(cracks, smooth(0.035, 0.0, d * r * 3.0) * smooth(rng.uniform(0.5, 0.95), 0.1, r))
            ring = np.exp(-((rr - rng.uniform(0.25, 0.4)) ** 2) / 0.0008)
            col = np.stack([0.85 + 0 * u, 0.9 + 0 * u, 0.95 + 0 * u], 2) * (1 - hole[..., None])
            a = np.clip(hole + cracks * 0.8 + ring * 0.6 + smooth(0.25, 0.05, rr) * 0.4, 0, 1)
            h = cracks * 0.3 + ring * 0.3 - hole
        elif kind == "blood":
            blobs = np.zeros_like(u)
            for k in range(int(rng.integers(5, 12))):
                x, y = rng.normal(0, 0.28), rng.normal(0, 0.28)
                s = rng.uniform(0.04, 0.22)
                blobs = np.maximum(blobs, smooth(s, s * 0.5, np.sqrt((u - x) ** 2 + (v - y) ** 2)))
            core = smooth(0.4, 0.1, rr * (0.8 + 0.4 * n))
            a = np.clip(np.maximum(blobs, core) * (0.7 + 0.3 * n), 0, 1) * smooth(1.0, 0.8, r)
            col = np.stack([0.32 + 0.1 * n, 0.015 + 0 * u, 0.012 + 0 * u], 2)
            h = a * 0.15
        elif kind == "scorch":
            a = np.clip(smooth(0.9, 0.0, rr * (0.8 + 0.5 * n)) * 0.85, 0, 1)
            col = np.stack([0.05 + 0 * u, 0.045 + 0 * u, 0.04 + 0 * u], 2)
            h = -a * 0.1
        elif kind == "dirt":
            a = np.clip(smooth(0.7, 0.05, rr * (0.7 + 0.6 * n)) * (0.5 + 0.5 * n), 0, 1)
            col = np.stack([0.22 + 0 * u, 0.17 + 0 * u, 0.11 + 0 * u], 2)
            h = -smooth(0.3, 0.0, rr) * 0.8 + a * 0.2
        else:  # concrete / brick / plaster / tile / asphalt
            hole = smooth(0.15, 0.09, rr)
            crater = smooth(0.5, 0.12, rr * (0.85 + 0.5 * n))
            cracks = np.zeros_like(u)
            for k in range(int(rng.integers(3, 7))):
                a0 = rng.uniform(0, 2 * math.pi)
                d = np.abs(np.angle(np.exp(1j * (ang - a0))))
                cracks = np.maximum(cracks, smooth(0.04, 0.0, d * r * 2.0) * smooth(rng.uniform(0.45, 0.85), 0.15, r) * (r > 0.12))
            base = 0.5 if kind != "brick" else 0.42
            col = np.stack([base + 0.08 * n, base * 0.97 + 0.08 * n, base * 0.94 + 0.08 * n], 2)
            if kind == "brick":
                col = col * np.array([1.15, 0.75, 0.6])
            col = col * (1 - 0.85 * hole[..., None]) * (1 - 0.25 * cracks[..., None])
            a = np.clip(hole + crater * (0.55 + 0.4 * n) + cracks * 0.7, 0, 1) * smooth(0.95, 0.65, r)
            h = -hole - crater * 0.45 - cracks * 0.25
        out.append(rgba(col, a))
        heights.append(h.astype(np.float32))
    return out, heights



# ------------------------------------------------------------------ flesh (blood decals, wounds)
# Real blood: very dark, desaturated red; wet centres slightly lighter, dried rims darker
# (coffee ring). Albedo in sRGB; height for the normal pass (thick drops stand up).

_BLOOD_WET = np.array([0.31, 0.04, 0.034], np.float32)
_BLOOD_RIM = np.array([0.17, 0.028, 0.022], np.float32)


def _blood_col(a, n, rim_w=0.25):
    rim = np.clip(1 - a / max(rim_w, 1e-3), 0, 1) * (a > 0.02)
    col = _BLOOD_WET[None, None, :] * (0.85 + 0.3 * n[..., None]) * (1 - rim[..., None]) + _BLOOD_RIM[None, None, :] * rim[..., None]
    return col


def splatter(cell, variants, seed, **_):
    """Directional splatter: main mass on the left, satellite drops and spines thrown to +u
    (the decal tangent points along the ray, so stretched splats smear the same way)."""
    out, heights = [], []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    for i in range(variants):
        n = fbm(u * 5 + i * 3.1, v * 5, 0, 5, seed + i)
        a = np.zeros_like(u)
        h = np.zeros_like(u)
        cx, cy = rng.uniform(-0.45, -0.2), rng.uniform(-0.12, 0.12)
        rr = np.sqrt(((u - cx) / 1.25) ** 2 + (v - cy) ** 2) * (0.8 + 0.45 * n)
        core = smooth(rng.uniform(0.28, 0.42), 0.12, rr)
        a = np.maximum(a, core)
        h = np.maximum(h, core * 0.5)
        for k in range(int(rng.integers(10, 26))):
            d = rng.uniform(0.15, 1.25)
            ang = rng.normal(0, 0.45)
            x, y = cx + math.cos(ang) * d, cy + math.sin(ang) * d * 0.8
            s = rng.uniform(0.012, 0.07) * (1.15 - d * 0.5)
            el = 1 + d * rng.uniform(0.5, 2.5)
            c, sn = math.cos(ang), math.sin(ang)
            pu, pv = (u - x) * c + (v - y) * sn, -(u - x) * sn + (v - y) * c
            drop = smooth(s, s * 0.45, np.sqrt((pu / el) ** 2 + pv ** 2))
            a = np.maximum(a, drop)
            h = np.maximum(h, drop * 0.8)
            if rng.uniform() < 0.35:   # spine back to the mass
                spine = smooth(s * 0.35, 0.0, np.abs(pv)) * (pu < 0) * (pu > -d * 0.5) * smooth(-d * 0.5, -d * 0.2, pu)
                a = np.maximum(a, spine * 0.8)
        a = np.clip(a * (0.75 + 0.35 * n), 0, 1) * smooth(1.0, 0.85, np.maximum(np.abs(u), np.abs(v)))
        out.append(rgba(_blood_col(a, n), a))
        heights.append((h * a).astype(np.float32) * 0.6)
    return out, heights


def drip_strip(cell, variants, seed, **_):
    """Run down a wall: source blob at u = -1 (top), a tapering trail to +u, a bulb at the end."""
    out, heights = [], []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    for i in range(variants):
        n = fbm(u * 3 + i, v * 9, 0, 4, seed + i)
        wob = (fbm(u * 2.5 + i * 7, v * 0.2, 0, 3, seed + 40 + i) - 0.5) * 0.12
        end = rng.uniform(0.55, 0.85)
        width = 0.11 * (1 - 0.55 * (u + 1) / 2)
        trail = smooth(width, width * 0.5, np.abs(v - wob)) * (u < end)
        head = smooth(0.3, 0.12, np.sqrt(((u + 0.72) / 1.3) ** 2 + (v - wob) ** 2) * (0.85 + 0.3 * n))
        bulb = smooth(0.1, 0.05, np.sqrt((u - end) ** 2 + (v - wob) ** 2 * 1.4))
        a = np.clip(np.maximum(np.maximum(trail, head), bulb) * (0.8 + 0.3 * n), 0, 1)
        out.append(rgba(_blood_col(a, n, 0.35), a))
        heights.append((np.maximum(trail * 0.5, bulb) * a).astype(np.float32) * 0.6)
    return out, heights


def pool_mask(cell, variants, seed, **_):
    """Pool: a thick, smooth edged irregular mass (grows from small to large in the game)."""
    out, heights = [], []
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)
    for i in range(variants):
        n = fbm(u * 2 + i * 1.7, v * 2, 0, 4, seed + i)
        lobes = 0.12 * np.sin(ang * (3 + i) + i) + 0.08 * np.sin(ang * (5 + i * 2) + 1.3 * i)
        edge = 0.72 + lobes + (n - 0.5) * 0.3
        a = smooth(edge, edge - 0.05, r)
        col = _blood_col(a, n * 0.4 + 0.3, 0.08) * (0.8 + 0.2 * smooth(0.0, 0.6, r))[..., None]
        out.append(rgba(col, a))
        heights.append((smooth(edge, edge - 0.12, r) * 0.6).astype(np.float32))
    return out, heights


def wound(cell, variants, seed, kind="entry", **_):
    """Bullet wounds: entry = small dark hole + abrasion ring; exit = larger ragged tear;
    slash = a long cut along u."""
    out, heights = [], []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)
    for i in range(variants):
        n = fbm(u * 7 + i * 2.3, v * 7, 0, 5, seed + i)
        if kind == "entry":
            jag = 1 + 0.15 * (fbm(np.cos(ang) * 2 + i, np.sin(ang) * 2, 0, 3, seed + 9 + i) - 0.5)
            rr = r * jag
            hole = smooth(0.17, 0.11, rr)
            ring = smooth(0.42, 0.2, rr) * (1 - hole)
            bruise = smooth(0.95, 0.3, rr * (0.8 + 0.4 * n)) * (1 - ring) * (1 - hole)
            col = (np.array([0.05, 0.01, 0.01])[None, None] * hole[..., None] + np.array([0.36, 0.08, 0.06])[None, None] * ring[..., None]
                   + np.array([0.24, 0.1, 0.1])[None, None] * bruise[..., None])
            a = np.clip(hole + ring * 0.95 + bruise * 0.15, 0, 1) * smooth(1.0, 0.8, r)
            h = -hole * 0.8 + ring * 0.15
        elif kind == "slash":
            wob = (fbm(u * 2 + i * 3, v * 0.1, 0, 3, seed + 5 + i) - 0.5) * 0.15
            w = 0.07 * (1 - u * u) + 0.01
            cut = smooth(w, w * 0.3, np.abs(v - wob)) * smooth(0.95, 0.75, np.abs(u))
            blood = smooth(w * 3.5, w, np.abs(v - wob) * (0.8 + 0.4 * n)) * smooth(1.0, 0.6, np.abs(u)) * (1 - cut)
            col = np.array([0.12, 0.012, 0.01])[None, None] * cut[..., None] + _BLOOD_WET[None, None] * blood[..., None]
            a = np.clip(cut + blood * 0.8, 0, 1)
            h = -cut * 0.6 + blood * 0.1
        else:
            k = int(rng.integers(4, 7))
            star = 0.25 + 0.18 * np.abs(np.sin(ang * k * 0.5 + rng.uniform(0, 3))) + (fbm(np.cos(ang) * 3 + i, np.sin(ang) * 3, 0, 4, seed + 21 + i) - 0.5) * 0.25
            tear = smooth(star, star - 0.06, r)
            tissue = tear * (fbm(u * 14 + i, v * 14, 0, 3, seed + 33 + i) > 0.55)
            blood = smooth(0.85, 0.3, r * (0.75 + 0.5 * n)) * (1 - tear)
            col = (np.array([0.16, 0.015, 0.012])[None, None] * tear[..., None] * (1 - tissue[..., None]) + np.array([0.5, 0.16, 0.14])[None, None] * tissue[..., None]
                   + _BLOOD_WET[None, None] * blood[..., None])
            a = np.clip(tear + blood * 0.85, 0, 1) * smooth(1.0, 0.8, r)
            h = -tear * 0.4 + tissue * 0.5 + blood * 0.15
        out.append(rgba(col, a))
        heights.append(h.astype(np.float32))
    return out, heights


def stain(cell, variants, seed, cloth=False, **_):
    """Blood soaking into skin or cloth: soft irregular blob, darker wicking edge; cloth shows
    the weave."""
    out, heights = [], []
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    for i in range(variants):
        n = fbm(u * 3 + i * 4.1, v * 3, 0, 5, seed + i)
        edge = 0.65 + (n - 0.5) * 0.55
        a = smooth(edge, edge - 0.25, r)
        wick = np.exp(-((r - edge + 0.08) ** 2) / 0.004) * 0.5
        if cloth:
            weave = 0.85 + 0.15 * np.sin(u * cell * 0.9) * np.sin(v * cell * 0.9)
            a = a * weave
        a = np.clip(a * 0.9 + wick * a, 0, 1)
        col = _blood_col(a, n, 0.15) * (1 - 0.3 * wick[..., None])
        out.append(rgba(col, a))
        heights.append((a * 0.05).astype(np.float32))
    return out, heights


def strand(cell, variants, seed, **_):
    """Stretched blood strand: along v, tapering, a drop at the head."""
    out = []
    u, v = grid(cell)
    for i in range(variants):
        wob = (fbm(v * 2 + i * 5, u * 0.1, 0, 3, seed + i) - 0.5) * 0.2
        w = 0.12 * (0.35 + 0.65 * (v + 1) / 2)
        line = smooth(w, w * 0.4, np.abs(u - wob)) * smooth(-0.95, -0.7, v)
        head = smooth(0.2, 0.1, np.sqrt((u - wob) ** 2 + (v - 0.75) ** 2))
        a = np.clip(np.maximum(line, head), 0, 1)
        out.append(rgba(_BLOOD_WET * 0.9, a))
    return out


def flash_petal(cell, variants, seed, **_):
    """Secondary flash seen from the side (axis / cross quads): the fireball blooms out of the bore
    line at the bottom of the image, a petal / cone of flame with ragged orange edges towards the
    top. 16 shapes so consecutive shots never repeat (docs/BALLISTICS.md "Muzzle flash")."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    t = (1 - v) * 0.5          # 0 at the muzzle (bottom) .. 1 at the far end (top)
    for i in range(variants):
        start = rng.uniform(0.04, 0.16)         # visible gap to the shock bottle
        peak = rng.uniform(0.35, 0.6)           # where the ball is widest
        width = rng.uniform(0.32, 0.55)
        lobes = int(rng.integers(2, 5))
        phase = rng.uniform(0, 2 * math.pi)
        prof = np.clip((t - start) / max(peak - start, 1e-3), 0, 1) ** 0.6 * np.clip((1.0 - t) / max(1.0 - peak, 1e-3), 0, 1) ** 0.8
        lobe = 1.0 + 0.25 * np.sin(t * lobes * math.pi + phase)
        n = fbm(u * 5 + i * 3.7, t * 7 - i * 1.3, 0.0, 4, seed + i * 11)
        w = width * prof * lobe + 1e-3
        edge = np.abs(u) / np.maximum(w * (0.75 + 0.5 * n), 1e-3)
        body = smooth(1.0, 0.25, edge) * smooth(start - 0.02, start + 0.05, t)
        heat = np.clip(body * (0.55 + 0.6 * n) * (1.25 - 0.55 * t), 0, 1)
        out.append(rgba(heat_ramp(heat * 0.95), np.clip(heat * 1.6, 0, 1)))
    return out


def flash_mach(cell, variants, seed, **_):
    """Intermediate flash: the bright disc behind the Mach disk of the gas jet (a white-yellow core
    with a few short radial streaks), seen on a cross quad."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    r = np.sqrt(u * u + v * v)
    ang = np.arctan2(v, u)
    for i in range(variants):
        n = int(rng.integers(6, 11))
        ph = rng.uniform(0, 2 * math.pi)
        streak = np.zeros_like(r)
        for k in range(n):
            d = np.angle(np.exp(1j * (ang - ph - k * 2 * math.pi / n - rng.uniform(-0.2, 0.2))))
            streak = np.maximum(streak, np.exp(-(d * d) / 0.004) * smooth(rng.uniform(0.5, 0.9), 0.1, r))
        core = np.exp(-(r * r) / 0.05)
        heat = np.clip(core * 1.2 + streak * 0.5, 0, 1)
        out.append(rgba(heat_ramp(0.55 + 0.45 * heat), np.clip(heat * 1.3, 0, 1) * smooth(1.0, 0.8, r)))
    return out


def flash_prong(cell, variants, seed, **_):
    """Flash hider remnant: thin dim spikes of flame from between the prongs (axis quads)."""
    out = []
    rng = np.random.default_rng(seed)
    u, v = grid(cell)
    t = (1 - v) * 0.5
    for i in range(variants):
        length = rng.uniform(0.55, 0.95)
        w = 0.06 * (1 - t / length).clip(0, 1) + 0.005
        n = fbm(u * 6 + i, t * 9, 0.0, 3, seed + i * 3)
        body = smooth(1.0, 0.3, np.abs(u) / (w * (0.8 + 0.4 * n))) * smooth(length, length * 0.6, t)
        heat = np.clip(body * (0.6 + 0.4 * n), 0, 1)
        out.append(rgba(heat_ramp(heat * 0.85), np.clip(heat * 1.4, 0, 1)))
    return out


def tracer_head(cell, variants=1, seed=0, **_):
    """Tracer head along the image vertical (stretched): a white-hot core fading into the
    compound's colour (tinted per round) towards the tail at the bottom."""
    u, v = grid(cell)
    t = (1 - v) * 0.5          # 0 tail .. 1 head
    core = np.exp(-(u * u) / 0.006) * smooth(0.0, 0.85, t) * smooth(1.0, 0.92, t)
    glow = np.exp(-(u * u) / 0.06) * smooth(0.0, 0.9, t) * smooth(1.0, 0.85, t) * 0.5
    a = np.clip(core + glow, 0, 1)
    col = np.where((core > 0.6)[..., None], np.array([1, 1, 1], np.float32), np.array([1, 1, 1], np.float32) * 0.9)
    return [rgba(col, a)]

GENERATORS = {
    "flash_petal": flash_petal,
    "flash_mach": flash_mach,
    "flash_prong": flash_prong,
    "tracer_head": tracer_head,
    "flash_core": flash_core,
    "flash_side": flash_side,
    "flash_suppressed": flash_suppressed,
    "spark": spark,
    "ember": ember,
    "glow": glow,
    "puff": puff,
    "mist": mist,
    "splash": splash,
    "ripple": ripple,
    "chunks": chunks,
    "drops": drops,
    "grass_bits": grass_bits,
    "heat_normal": heat_normal,
    "decal_hole": decal_hole,
    "splatter": splatter,
    "drip_strip": drip_strip,
    "pool_mask": pool_mask,
    "wound": wound,
    "stain": stain,
    "strand": strand,
}
