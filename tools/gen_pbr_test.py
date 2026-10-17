#!/usr/bin/env python3
"""Generates the glTF PBR test models of the dev room "PBR shelf" (docs/PBR.md); no third-party packages.

  assets/models/pbr/pbr_ball.gltf      UV sphere with the full map set: base colour (sRGB), ORM
                                       (r occlusion, g roughness, b metallic: occlusionTexture and
                                       metallicRoughnessTexture share it), tangent-space normal,
                                       emissive (sRGB) + KHR_materials_emissive_strength
  assets/models/pbr/pbr_*.png          its textures (512 x 256, lat-long)
  assets/models/pbr/pbr_swatches.gltf  2 x 5 untextured spheres: gold metal (top) and red plastic
                                       (bottom), roughness 0.05 .. 1 left to right
  assets/models/pbr/pbr_mask.gltf      a doubleSided alphaMode MASK grille (base colour PNG embedded
                                       as a data: uri) on an opaque stand

The ball's panels: painted orange (dielectric, rough) and bare brushed steel (metal, smooth) in a
checkerboard, recessed seams (normal + occlusion), rivets at the panel corners, a cyan emissive strip
along the equator.

Run from the repo root:  python tools/gen_pbr_test.py
"""
import base64
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models", "pbr")
W, H = 512, 256          # texture size (u around, v top to bottom)
PANELS_U, PANELS_V = 16, 8


def png_bytes(w, h, rows):
    """rows: h lists of w (r, g, b, a) tuples of ints 0..255."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw.extend(px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def write(name, data):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def to8(x):
    return max(0, min(255, int(round(x * 255.0))))


def srgb8(x):
    x = max(0.0, min(1.0, x))
    return to8(x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055)


def hash2(x, y):
    n = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


# ----------------------------------------------------------------------------- ball maps

PW, PH = W // PANELS_U, H // PANELS_V     # panel size in texels (32 x 32)
SEAM = 1.6                                # half width of the seam groove (texels)
STRIP_V = H // 2                          # equator row (the emissive strip)


def panel_height(x, y):
    """Height (0 flat, < 0 recessed, > 0 raised) and what is there: 'seam', 'rivet', 'strip' or None."""
    lx, ly = x % PW, y % PH
    # seams at the panel edges (distance to the nearest edge)
    d = min(lx, PW - lx, ly, PH - ly)
    if abs(y - STRIP_V) <= 2.5:
        return -0.6, "strip"
    if d < SEAM + 1.0:
        t = max(0.0, min(1.0, (d - SEAM) / 1.0))
        return -1.0 * (1.0 - t * t * (3 - 2 * t)), "seam" if d < SEAM else None
    # rivets: hemispheres (radius 2.6 texels) 5 texels in from each corner
    for cx in (5.0, PW - 5.0):
        for cy in (5.0, PH - 5.0):
            r2 = (lx + 0.5 - cx) ** 2 + (ly + 0.5 - cy) ** 2
            if r2 < 2.6 * 2.6:
                return 0.9 * math.sqrt(1.0 - r2 / (2.6 * 2.6)), "rivet"
    return 0.0, None


def gen_ball_maps():
    hgt = [[0.0] * W for _ in range(H)]
    kind = [[None] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            hgt[y][x], kind[y][x] = panel_height(x, y)
    albedo, orm, normal, emissive = [], [], [], []
    for y in range(H):
        ra, ro, rn, re = [], [], [], []
        for x in range(W):
            k = kind[y][x]
            painted = ((x // PW) + (y // PH)) % 2 == 0
            # paint wear: bare metal shows through in scratches near the panel borders
            lx, ly = x % PW, y % PH
            wear = hash2(x // 2, y // 2) > 0.93 and min(lx, PW - lx, ly, PH - ly) < 7
            brushed = 0.5 + 0.5 * hash2(y * 7 + x // 64, 3)       # streaks along u
            if k == "seam" or k == "strip":
                alb, rough, metal = (0.04, 0.04, 0.045), 0.85, 0.0
            elif k == "rivet":
                alb, rough, metal = (0.75, 0.74, 0.72), 0.3, 1.0
            elif painted and not wear:
                alb, rough, metal = (0.80, 0.30, 0.06), 0.55 + 0.1 * hash2(x // 4, y // 4), 0.0
            else:
                alb, rough, metal = (0.86, 0.87, 0.89), 0.14 + 0.16 * brushed, 1.0
            # occlusion: the seams, the rim around rivets and the strip channel
            ao = 1.0
            h = hgt[y][x]
            if h < 0:
                ao = 0.35 + 0.65 * (1.0 + h)
            else:
                ao = 1.0
                for dy in (-3, 0, 3):
                    for dx in (-3, 0, 3):
                        if hgt[y + dy if 0 <= y + dy < H else y][(x + dx) % W] > 0.3 and h == 0:
                            ao = min(ao, 0.72)
            # tangent-space normal (glTF: +x = +u, +y = image up = -v): from the height gradient
            hx = hgt[y][(x + 1) % W] - hgt[y][(x - 1) % W]
            hy = hgt[min(y + 1, H - 1)][x] - hgt[max(y - 1, 0)][x]   # along +v (image down)
            s = 1.2
            nx, ny, nz = -hx * s, hy * s, 1.0
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            ra.append((srgb8(alb[0]), srgb8(alb[1]), srgb8(alb[2]), 255))
            ro.append((to8(ao), to8(rough), to8(metal), 255))
            rn.append((to8(nx * inv * 0.5 + 0.5), to8(ny * inv * 0.5 + 0.5), to8(nz * inv * 0.5 + 0.5), 255))
            # emissive: the strip, in segments (one gap per panel)
            on = k == "strip" and abs(y - STRIP_V) <= 1.5 and 3 <= lx <= PW - 4
            re.append((srgb8(0.15), srgb8(0.85), srgb8(1.0), 255) if on else (0, 0, 0, 255))
        albedo.append(ra)
        orm.append(ro)
        normal.append(rn)
        emissive.append(re)
    write("pbr_ball_albedo.png", png_bytes(W, H, albedo))
    write("pbr_ball_orm.png", png_bytes(W, H, orm))
    write("pbr_ball_normal.png", png_bytes(W, H, normal))
    write("pbr_ball_emissive.png", png_bytes(W, H, emissive))


# ----------------------------------------------------------------------------- meshes


def uv_sphere(radius, stacks, slices):
    """Positions / normals / uvs / indices; u = +x to the right seen from +z, v = 0 at the top."""
    pos, nrm, uv, idx = [], [], [], []
    for i in range(stacks + 1):
        th = math.pi * i / stacks
        for j in range(slices + 1):
            ph = 2.0 * math.pi * j / slices
            n = (math.sin(th) * math.sin(ph), math.cos(th), math.sin(th) * math.cos(ph))
            pos.append(tuple(c * radius for c in n))
            nrm.append(n)
            uv.append((j / slices, i / stacks))
    for i in range(stacks):
        for j in range(slices):
            a, b = i * (slices + 1) + j, (i + 1) * (slices + 1) + j
            c, d = b + 1, a + 1
            if i != 0:
                idx += [a, b, d]
            if i != stacks - 1:
                idx += [b, c, d]
    return pos, nrm, uv, idx


def quad(w, h, y0):
    """Vertical quad in the xy plane facing +z, uv v = 0 at the top."""
    pos = [(-w / 2, y0 + h, 0), (-w / 2, y0, 0), (w / 2, y0, 0), (w / 2, y0 + h, 0)]
    nrm = [(0, 0, 1)] * 4
    uv = [(0, 0), (0, 1), (1, 1), (1, 0)]
    return pos, nrm, uv, [0, 1, 2, 0, 2, 3]


def box(sx, sy, sz, y0):
    pos, nrm, uv, idx = [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (1, 0, 0), (0, 0, -1)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    for n, t, b in faces:
        base = len(pos)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [n[k] * (hx, hy, hz)[k] + t[k] * su * (hx, hy, hz)[k] + b[k] * sv * (hx, hy, hz)[k] for k in range(3)]
            p[1] += y0 + hy
            pos.append(tuple(p))
            nrm.append(n)
            uv.append(((su + 1) / 2, (1 - sv) / 2))
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


class Gltf:
    """Minimal glTF 2.0 writer: one embedded buffer, meshes of POSITION / NORMAL / TEXCOORD_0."""

    def __init__(self):
        self.buf = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "deadground gen_pbr_test.py"}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "materials": [], "accessors": [], "bufferViews": []}

    def _view(self, data, target):
        while len(self.buf) % 4:
            self.buf.append(0)
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": len(self.buf), "byteLength": len(data), "target": target})
        self.buf.extend(data)
        return len(self.doc["bufferViews"]) - 1

    def _accessor(self, values, kind):
        n = len(values[0]) if kind != "SCALAR" else 1
        if kind == "SCALAR":
            data = struct.pack("<%dI" % len(values), *values)
            acc = {"componentType": 5125, "count": len(values), "type": "SCALAR", "bufferView": self._view(data, 34963)}
        else:
            flat = [c for v in values for c in v]
            data = struct.pack("<%df" % len(flat), *flat)
            acc = {"componentType": 5126, "count": len(values), "type": kind, "bufferView": self._view(data, 34962)}
            if kind == "VEC3":
                acc["min"] = [min(v[k] for v in values) for k in range(n)]
                acc["max"] = [max(v[k] for v in values) for k in range(n)]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def geometry(self, geo):
        pos, nrm, uv, idx = geo
        return {"POSITION": self._accessor(pos, "VEC3"), "NORMAL": self._accessor(nrm, "VEC3"), "TEXCOORD_0": self._accessor(uv, "VEC2")}, self._accessor(idx, "SCALAR")

    def material(self, m):
        self.doc["materials"].append(m)
        return len(self.doc["materials"]) - 1

    def node(self, name, prims, translation=(0, 0, 0)):
        self.doc["meshes"].append({"name": name, "primitives": [{"attributes": a, "indices": i, "material": m} for (a, i), m in prims]})
        self.doc["nodes"].append({"name": name, "mesh": len(self.doc["meshes"]) - 1, "translation": list(translation)})
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]) - 1)

    def save(self, name):
        import json
        self.doc["buffers"] = [{"byteLength": len(self.buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(self.buf)).decode()}]
        for k in ("images", "textures", "samplers"):
            if k in self.doc and not self.doc[k]:
                del self.doc[k]
        write(name, json.dumps(self.doc, indent=1).encode())


def gen_ball():
    g = Gltf()
    g.doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    g.doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 33071}]
    g.doc["images"] = [{"uri": "pbr_ball_albedo.png"}, {"uri": "pbr_ball_orm.png"}, {"uri": "pbr_ball_normal.png"}, {"uri": "pbr_ball_emissive.png"}]
    g.doc["textures"] = [{"sampler": 0, "source": i} for i in range(4)]
    m = g.material({
        "name": "pbr_ball",
        "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 0}, "metallicFactor": 1.0, "roughnessFactor": 1.0,
                                 "metallicRoughnessTexture": {"index": 1}},
        "normalTexture": {"index": 2, "scale": 1.0},
        "occlusionTexture": {"index": 1, "strength": 1.0},
        "emissiveTexture": {"index": 3},
        "emissiveFactor": [1.0, 1.0, 1.0],
        "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 4.0}},
    })
    g.node("pbr_ball", [(g.geometry(uv_sphere(0.35, 48, 96)), m)])
    g.save("pbr_ball.gltf")


def gen_swatches():
    g = Gltf()
    geo = g.geometry(uv_sphere(0.17, 24, 48))
    roughs = [0.05, 0.25, 0.45, 0.7, 1.0]
    for row, (label, color, metal) in enumerate((("gold", [1.0, 0.78, 0.34, 1.0], 1.0), ("plastic", [0.65, 0.07, 0.05, 1.0], 0.0))):
        for k, r in enumerate(roughs):
            m = g.material({"name": "%s_r%02d" % (label, int(r * 100)), "pbrMetallicRoughness": {"baseColorFactor": color, "metallicFactor": metal, "roughnessFactor": r}})
            g.node("%s_%d" % (label, k), [(geo, m)], ((k - 2) * 0.42, 0.17 + (1 - row) * 0.42, 0.0))
    g.save("pbr_swatches.gltf")


def gen_mask():
    # base colour + alpha: a green grille with round holes (alpha 0), embedded as a data: uri
    n = 128
    rows = []
    for y in range(n):
        row = []
        for x in range(n):
            cx, cy = (x % 32) - 15.5, (y % 32) - 15.5
            hole = cx * cx + cy * cy < 11.0 * 11.0
            edge = x < 4 or y < 4 or x >= n - 4 or y >= n - 4
            a = 0 if hole and not edge else 255
            row.append((srgb8(0.18), srgb8(0.5), srgb8(0.16), a))
        rows.append(row)
    g = Gltf()
    g.doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987}]
    g.doc["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(png_bytes(n, n, rows)).decode()}]
    g.doc["textures"] = [{"sampler": 0, "source": 0}]
    grille = g.material({"name": "grille", "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True,
                         "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}})
    stand = g.material({"name": "stand", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.2, 0.22, 1], "metallicFactor": 1.0, "roughnessFactor": 0.4}})
    g.node("grille", [(g.geometry(quad(0.6, 0.6, 0.05)), grille)])
    g.node("stand", [(g.geometry(box(0.5, 0.05, 0.2, 0.0)), stand)])
    g.save("pbr_mask.gltf")


if __name__ == "__main__":
    gen_ball_maps()
    gen_ball()
    gen_swatches()
    gen_mask()
    print("wrote", os.path.normpath(OUT))
