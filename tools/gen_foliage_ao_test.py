#!/usr/bin/env python3
"""Generates the alpha-tested foliage test models of the dev room "AO foliage corner"
(docs/RENDERING_POST.md "Foliage-aware AO"); no third-party packages.

  assets/models/foliage/foliage_bush.gltf   a 1.3 m bush: 22 leaf-cluster cards (alphaMode MASK, doubleSided,
                                            spherical "bush" normals) on an opaque stem
  assets/models/foliage/foliage_grass.gltf  a 1.6 x 1.6 m patch of 90 grass blade cards (MASK, doubleSided)

Every card vertex carries baked AO in its COLOR_0 alpha (darker inside the bush / at the blade roots);
the card materials say so in their glTF extras ("ao_vertex": true, src/render/material_ao.h) and keep
the foliage defaults otherwise (ao_strength 0.3, baked and screen-space AO combined as min). The stem
is a plain opaque material (full screen-space AO, multiplied). Textures are embedded as data: uris.

Run from the repo root:  python tools/gen_foliage_ao_test.py
"""
import base64
import json
import math
import os
import random
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "models", "foliage")


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


def srgb8(x):
    x = max(0.0, min(1.0, x))
    return max(0, min(255, int(round((x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055) * 255.0))))


def leaf_texture(size=256, seed=3):
    """A cluster of ~11 pointed leaves on a twig: sRGB greens, alpha 255 inside, 0 outside."""
    rnd = random.Random(seed)
    leaves = []
    for _ in range(11):
        a = rnd.uniform(0, 2 * math.pi)
        r = rnd.uniform(0.05, 0.25)
        cx, cy = 0.5 + r * math.cos(a), 0.5 + r * math.sin(a)
        leaves.append((cx, cy, rnd.uniform(0, math.pi), rnd.uniform(0.17, 0.24), rnd.uniform(0.06, 0.09), rnd.uniform(0.8, 1.15)))
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            u, v = (x + 0.5) / size, (y + 0.5) / size
            px = (0, 0, 0, 0)
            for cx, cy, ang, ln, wd, tone in leaves:
                du, dv = u - cx, v - cy
                lu = du * math.cos(ang) + dv * math.sin(ang)
                lv = -du * math.sin(ang) + dv * math.cos(ang)
                t = lu / ln                         # -1 .. 1 along the leaf
                if abs(t) >= 1.0:
                    continue
                half = wd * (1.0 - t * t) ** 0.8    # pointed ends
                if abs(lv) >= half:
                    continue
                vein = 1.0 - 0.25 * math.exp(-(lv / (wd * 0.08)) ** 2)
                edge = 0.8 + 0.2 * (1.0 - abs(lv) / half)
                g = (0.10 * tone * vein * edge, 0.26 * tone * vein * edge, 0.05 * tone * vein)
                px = (srgb8(g[0]), srgb8(g[1]), srgb8(g[2]), 255)
            row.append(px)
        rows.append(row)
    return png_bytes(size, size, rows)


def blade_texture(w=64, h=256):
    """Three tapered grass blades side by side (root at the bottom, v = 1)."""
    rows = []
    for y in range(h):
        v = (y + 0.5) / h                           # 0 top (tip) .. 1 root
        row = []
        for x in range(w):
            u = (x + 0.5) / w
            px = (0, 0, 0, 0)
            for c, lean, top in ((0.2, -0.06, 0.08), (0.5, 0.0, 0.18), (0.8, 0.07, 0.08)):
                if v < top:
                    continue
                s = (v - top) / (1 - top)           # 0 tip .. 1 root
                cx = c + lean * (1 - s)
                half = 0.11 * s ** 0.6
                if abs(u - cx) < half:
                    shade = 0.7 + 0.3 * s
                    px = (srgb8(0.12 * shade), srgb8(0.30 * shade), srgb8(0.07 * shade), 255)
            row.append(px)
        rows.append(row)
    return png_bytes(w, h, rows)


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / l for c in v]


class Prim:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.col, self.idx = [], [], [], [], []

    def vert(self, p, n, uv, col):
        self.pos.append(p)
        self.nrm.append(n)
        self.uv.append(uv)
        self.col.append(col)
        return len(self.pos) - 1

    def quad(self, a, b, c, d):
        self.idx += [a, b, c, a, c, d]


def bush(seed=7):
    rnd = random.Random(seed)
    leaves, stem = Prim(), Prim()
    centre, radius = [0.0, 0.72, 0.0], 0.5
    for i in range(22):
        # card centre inside an ellipsoid, random yaw / tilt, 0.5 .. 0.62 m wide
        while True:
            p = [rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-1, 1)]
            if sum(c * c for c in p) <= 1:
                break
        c = [centre[0] + p[0] * radius, centre[1] + p[1] * 0.42, centre[2] + p[2] * radius]
        yaw, tilt, s = rnd.uniform(0, math.pi), rnd.uniform(-0.6, 0.6), rnd.uniform(0.5, 0.62) * 0.5
        right = [math.cos(yaw), 0.0, math.sin(yaw)]
        # world up tilted about the card's horizontal axis (stays orthogonal to `right`)
        up = [-math.sin(yaw) * math.sin(tilt), math.cos(tilt), math.cos(yaw) * math.sin(tilt)]
        corners = [(-1, -1, 0, 1), (1, -1, 1, 1), (1, 1, 1, 0), (-1, 1, 0, 0)]
        ids = []
        for sx, sy, u, v in corners:
            q = [c[k] + right[k] * sx * s + up[k] * sy * s for k in range(3)]
            n = norm([q[0] - centre[0], (q[1] - centre[1]) * 0.8 + 0.25, q[2] - centre[2]])   # bush normals
            d = math.sqrt(sum((q[k] - centre[k]) ** 2 for k in range(3))) / (radius * 1.15)
            ao = max(0.35, min(1.0, 0.35 + 0.65 * d ** 1.4)) * (0.8 + 0.2 * min(1.0, q[1] / 1.1))   # baked: inside / low = dark
            ids.append(leaves.vert(q, n, [u, v], [1.0, 1.0, 1.0, ao]))
        leaves.quad(*ids)
    # opaque stem: a thin square post up into the bush
    h, r = 0.75, 0.03
    for k in range(4):
        a0, a1 = k * math.pi / 2 + math.pi / 4, (k + 1) * math.pi / 2 + math.pi / 4
        p0 = [math.cos(a0) * r, 0.0, math.sin(a0) * r]
        p1 = [math.cos(a1) * r, 0.0, math.sin(a1) * r]
        n = norm([math.cos((a0 + a1) / 2), 0.0, math.sin((a0 + a1) / 2)])
        ids = [stem.vert([p0[0], 0.0, p0[2]], n, [0, 1], [1, 1, 1, 1]), stem.vert([p0[0], h, p0[2]], n, [0, 0], [1, 1, 1, 1]),
               stem.vert([p1[0], h, p1[2]], n, [1, 0], [1, 1, 1, 1]), stem.vert([p1[0], 0.0, p1[2]], n, [1, 1], [1, 1, 1, 1])]
        stem.idx += [ids[0], ids[2], ids[1], ids[0], ids[3], ids[2]]
    return leaves, stem


def grass(seed=11):
    rnd = random.Random(seed)
    g = Prim()
    for i in range(90):
        x, z = rnd.uniform(-0.8, 0.8), rnd.uniform(-0.8, 0.8)
        yaw = rnd.uniform(0, math.pi)
        hgt, w = rnd.uniform(0.3, 0.55), rnd.uniform(0.07, 0.11)
        lean = [rnd.uniform(-0.08, 0.08), 0.0, rnd.uniform(-0.08, 0.08)]
        right = [math.cos(yaw) * w, 0.0, math.sin(yaw) * w]
        n = norm([0.0, 1.0, 0.0])   # blades lit from above on both sides (as the ground cover)
        root_ao, tip_ao = 0.45, 1.0
        ids = [g.vert([x - right[0], 0.0, z - right[2]], n, [0, 1], [1, 1, 1, root_ao]),
               g.vert([x + right[0], 0.0, z + right[2]], n, [1, 1], [1, 1, 1, root_ao]),
               g.vert([x + right[0] + lean[0], hgt, z + right[2] + lean[2]], n, [1, 0], [1, 1, 1, tip_ao]),
               g.vert([x - right[0] + lean[0], hgt, z - right[2] + lean[2]], n, [0, 0], [1, 1, 1, tip_ao])]
        g.quad(*ids)
    return g


def write_gltf(path, prims, materials, images):
    """prims: [(Prim, material index)]; materials: glTF material dicts; images: [png bytes]."""
    blob = bytearray()
    views, accessors, meshes_prims = [], [], []

    def add(data, fmt, count, gltf_type, comp, target, minmax=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        for d in data:
            blob.extend(struct.pack(fmt, *d) if isinstance(d, (list, tuple)) else struct.pack(fmt, d))
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(blob) - off, "target": target})
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": gltf_type}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    for p, mi in prims:
        mn = [min(v[k] for v in p.pos) for k in range(3)]
        mx = [max(v[k] for v in p.pos) for k in range(3)]
        a_pos = add(p.pos, "<3f", len(p.pos), "VEC3", 5126, 34962, (mn, mx))
        a_nrm = add(p.nrm, "<3f", len(p.nrm), "VEC3", 5126, 34962)
        a_uv = add(p.uv, "<2f", len(p.uv), "VEC2", 5126, 34962)
        a_col = add(p.col, "<4f", len(p.col), "VEC4", 5126, 34962)
        a_idx = add(p.idx, "<I", len(p.idx), "SCALAR", 5125, 34963)
        meshes_prims.append({"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv, "COLOR_0": a_col}, "indices": a_idx, "material": mi})
    gl = {
        "asset": {"version": "2.0", "generator": "tools/gen_foliage_ao_test.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": os.path.splitext(os.path.basename(path))[0]}],
        "meshes": [{"primitives": meshes_prims}],
        "materials": materials,
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(blob)).decode()}],
        "bufferViews": views, "accessors": accessors,
    }
    if images:
        gl["images"] = [{"uri": "data:image/png;base64," + base64.b64encode(im).decode()} for im in images]
        gl["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]
        gl["textures"] = [{"source": i, "sampler": 0} for i in range(len(images))]
    os.makedirs(OUT, exist_ok=True)
    with open(path, "w", newline="\n") as f:
        json.dump(gl, f, indent=1)
        f.write("\n")
    print("wrote", os.path.relpath(path))


def card_material(name, tex, extras):
    return {"name": name, "doubleSided": True, "alphaMode": "MASK", "alphaCutoff": 0.5,
            "pbrMetallicRoughness": {"baseColorTexture": {"index": tex}, "metallicFactor": 0.0, "roughnessFactor": 0.9},
            "extras": extras}


def main():
    leaves, stem = bush()
    write_gltf(os.path.join(OUT, "foliage_bush.gltf"), [(leaves, 0), (stem, 1)],
               [card_material("leaves", 0, {"ao_vertex": True}),
                {"name": "stem", "pbrMetallicRoughness": {"baseColorFactor": [0.22, 0.15, 0.09, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.85}}],
               [leaf_texture()])
    write_gltf(os.path.join(OUT, "foliage_grass.gltf"), [(grass(), 0)],
               [card_material("grass", 0, {"ao_vertex": True, "ao_strength": 0.3, "ao_combine": "min"})], [blade_texture()])


if __name__ == "__main__":
    main()
