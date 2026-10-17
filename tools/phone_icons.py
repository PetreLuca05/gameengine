#!/usr/bin/env python3
"""Phone OS icon atlas (docs/PHONE.md): assets/phone/icons.png, 128 px tiles in one row.

The same atlas is drawn on the in-game phone screen (src/gameplay/phone/phone_ui.c) and embedded
into the companion page (data URI, src/gameplay/phone/phone_os.c phone_page_build), so both
look identical. Tile names / order: TILES below, mirrored in assets/phone/apps.json "icons".

App icons are full colour (rounded square + white glyph). Weather glyphs are white with alpha:
the screen tints them with the theme's text colour.

Pure Python (stdlib only): shapes are signed distance functions, 4x4 supersampled.
    python tools/phone_icons.py [out.png]
"""
import math
import struct
import sys
import zlib

T = 128
SS = 4
TILES = ["camera", "gallery", "w_clear", "w_night", "w_partly", "w_cloudy", "w_rain", "w_storm", "w_snow", "w_fog"]


def clamp(x, a=0.0, b=1.0):
    return a if x < a else (b if x > b else x)


# ---------------------------------------------------------------- signed distances (pixels)
def d_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def d_rrect(px, py, cx, cy, hw, hh, r):
    qx, qy = abs(px - cx) - hw + r, abs(py - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def d_seg(px, py, ax, ay, bx, by, r):
    vx, vy, wx, wy = bx - ax, by - ay, px - ax, py - ay
    t = clamp((wx * vx + wy * vy) / max(vx * vx + vy * vy, 1e-9))
    return math.hypot(wx - vx * t, wy - vy * t) - r


def d_poly(px, py, pts):
    # Convex or concave polygon (even-odd), exact distance.
    d = 1e9
    inside = False
    n = len(pts)
    for i in range(n):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % n]
        d = min(d, d_seg(px, py, ax, ay, bx, by, 0))
        if (ay > py) != (by > py) and px < (bx - ax) * (py - ay) / (by - ay + 1e-12) + ax:
            inside = not inside
    return -d if inside else d


def cloud(px, py, cx, cy, s):
    return min(d_circle(px, py, cx - 14 * s, cy + 4 * s, 13 * s), d_circle(px, py, cx + 4 * s, cy - 6 * s, 18 * s),
               d_circle(px, py, cx + 20 * s, cy + 6 * s, 11 * s), d_rrect(px, py, cx + 2 * s, cy + 10 * s, 30 * s, 8 * s, 8 * s))


def sun(px, py, cx, cy, r, rays=True):
    d = d_circle(px, py, cx, cy, r)
    if rays:
        for k in range(8):
            a = k * math.pi / 4
            d = min(d, d_seg(px, py, cx + math.cos(a) * (r + 8), cy + math.sin(a) * (r + 8), cx + math.cos(a) * (r + 18), cy + math.sin(a) * (r + 18), 3.5))
    return d


# ---------------------------------------------------------------- tiles: (rgba at a pixel)
def over(dst, src):
    a = src[3] + dst[3] * (1 - src[3])
    if a <= 0:
        return (0, 0, 0, 0)
    return tuple((src[i] * src[3] + dst[i] * dst[3] * (1 - src[3])) / a for i in range(3)) + (a,)


def cov(d):
    return clamp(0.5 - d)


def tile_camera(x, y):
    c = (0, 0, 0, 0)
    bg = d_rrect(x, y, 64, 64, 58, 58, 26)
    k = cov(bg)
    if k > 0:
        g = 0.32 - 0.12 * (y / T)
        c = over(c, (g, g + 0.01, g + 0.03, k))
    body = min(d_rrect(x, y, 64, 70, 38, 26, 9), d_rrect(x, y, 54, 42, 14, 8, 4))
    lens_ring = abs(d_circle(x, y, 64, 70, 16)) - 4.5
    glyph = max(body, -d_circle(x, y, 64, 70, 21))
    c = over(c, (1, 1, 1, cov(glyph) * k))
    c = over(c, (1, 1, 1, cov(lens_ring) * k))
    c = over(c, (1.0, 0.84, 0.04, cov(d_circle(x, y, 92, 54, 4)) * k))
    return c


def tile_gallery(x, y):
    c = (0, 0, 0, 0)
    bg = d_rrect(x, y, 64, 64, 58, 58, 26)
    k = cov(bg)
    if k > 0:
        t = y / T
        c = over(c, (0.98 - 0.1 * t, 0.62 + 0.15 * t, 0.25 + 0.45 * t, k))
    frame = d_rrect(x, y, 64, 66, 36, 30, 8)
    c = over(c, (1, 1, 1, cov(frame) * k))
    inner = d_rrect(x, y, 64, 66, 30, 24, 4)
    clip = cov(inner) * k
    if clip > 0:
        c = over(c, (0.55, 0.78, 0.98, clip))
        c = over(c, (1.0, 0.85, 0.2, cov(d_circle(x, y, 80, 54, 7)) * clip))
        m1 = d_poly(x, y, [(34, 90), (58, 60), (78, 90)])
        m2 = d_poly(x, y, [(56, 90), (78, 68), (96, 90)])
        c = over(c, (0.20, 0.55, 0.32, cov(m2) * clip))
        c = over(c, (0.12, 0.42, 0.26, cov(m1) * clip))
    return c


def white(d):
    return (1, 1, 1, cov(d))


def tile_w_clear(x, y):
    return white(sun(x, y, 64, 64, 22))


def tile_w_night(x, y):
    moon = max(d_circle(x, y, 64, 64, 30), -d_circle(x, y, 80, 52, 26))
    star = d_circle(x, y, 94, 88, 4)
    return white(min(moon, star))


def tile_w_partly(x, y):
    s = sun(x, y, 48, 46, 16)
    cl = cloud(x, y, 70, 78, 1.0)
    d = min(max(s, -(cl - 6)), cl)
    return white(d)


def tile_w_cloudy(x, y):
    return white(cloud(x, y, 62, 62, 1.25))


def drops(x, y, n, x0, dx, y0, ln):
    d = 1e9
    for i in range(n):
        ax = x0 + i * dx
        d = min(d, d_seg(x, y, ax, y0, ax - 6, y0 + ln, 3.5))
    return d


def tile_w_rain(x, y):
    return white(min(cloud(x, y, 62, 50, 1.1), drops(x, y, 4, 40, 16, 84, 18)))


def tile_w_storm(x, y):
    bolt = d_poly(x, y, [(66, 76), (50, 100), (62, 100), (54, 122), (80, 92), (67, 92), (76, 76)])
    return white(min(max(cloud(x, y, 62, 50, 1.1), -(bolt - 5)), bolt))


def tile_w_snow(x, y):
    d = cloud(x, y, 62, 48, 1.1)
    for (cx, cy) in [(42, 92), (64, 102), (86, 92), (52, 118), (76, 118)]:
        for k in range(3):
            a = k * math.pi / 3
            d = min(d, d_seg(x, y, cx - math.cos(a) * 6, cy - math.sin(a) * 6, cx + math.cos(a) * 6, cy + math.sin(a) * 6, 1.8))
    return white(d)


def tile_w_fog(x, y):
    d = 1e9
    for i, (a, b) in enumerate([(24, 92), (36, 104), (20, 84), (32, 96)]):
        yy = 40 + i * 16
        d = min(d, d_seg(x, y, a, yy, b, yy, 4.5))
    return white(d)


FUNCS = {n: globals()["tile_" + n] for n in TILES}


def render():
    W, H = T * len(TILES), T
    rows = []
    for y in range(H):
        row = bytearray()
        for ti, name in enumerate(TILES):
            f = FUNCS[name]
            for x in range(T):
                acc = [0.0, 0.0, 0.0, 0.0]
                for sy in range(SS):
                    for sx in range(SS):
                        r, g, b, a = f(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS)
                        acc[0] += r * a
                        acc[1] += g * a
                        acc[2] += b * a
                        acc[3] += a
                n = SS * SS
                a = acc[3] / n
                if a > 0:
                    px = [acc[i] / acc[3] for i in range(3)]
                else:
                    px = [1.0, 1.0, 1.0]
                row += bytes([int(clamp(px[0]) * 255 + 0.5), int(clamp(px[1]) * 255 + 0.5), int(clamp(px[2]) * 255 + 0.5), int(clamp(a) * 255 + 0.5)])
        rows.append(bytes(row))
    return W, H, rows


def write_png(path, W, H, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/phone/icons.png"
    W, H, rows = render()
    write_png(out, W, H, rows)
    print("wrote", out, W, "x", H, "tiles:", ", ".join(TILES))
