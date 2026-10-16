#!/usr/bin/env python3
"""Procedural placeholder vehicle meshes (docs/VEHICLES.md) -> assets/vehicles/*.gltf

  car_body.gltf     hatchback shell: hollow cabin (readable from the driver's seat), glass,
                    bumpers, lights (emissive), interior (seats, dashboard, floor)
  car_wheel.gltf    tire + rim, axle along X, centred on the origin
  car_steering.gltf steering wheel: rim in the local XZ plane, steering axis = local +Y
  moto_body.gltf    frame, tank, seat, exhaust, lights
  moto_wheel.gltf   tire + spoked rim
  moto_bar.gltf     handlebar + fork tubes: steering axis = local +Y, bar along X

Vehicle space: origin on the ground under the body centre, forward -Z, up +Y, right +X; the
numbers match the physics boxes / sockets in src/gameplay/vehicle_def.c and assets/vehicles/*.json.
Materials named "paint*" are tinted with the vehicle colour at runtime.

Run from the repo root:  python tools/gen_vehicle_mesh.py
"""
import base64
import json
import math
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "vehicles")

MATS = {
    "paint": ((1.0, 1.0, 1.0, 1.0), 0.15, 0.35, (0, 0, 0)),
    "paint_dark": ((0.55, 0.55, 0.55, 1.0), 0.15, 0.4, (0, 0, 0)),
    "glass": ((0.55, 0.62, 0.66, 0.22), 0.0, 0.05, (0, 0, 0)),   # translucent (alpha < 1: the blended pass)
    "plastic": ((0.06, 0.06, 0.065, 1.0), 0.0, 0.7, (0, 0, 0)),
    "rubber": ((0.035, 0.035, 0.035, 1.0), 0.0, 0.9, (0, 0, 0)),
    "metal": ((0.62, 0.63, 0.65, 1.0), 0.9, 0.3, (0, 0, 0)),
    "chrome": ((0.85, 0.86, 0.88, 1.0), 1.0, 0.12, (0, 0, 0)),
    "fabric": ((0.13, 0.12, 0.11, 1.0), 0.0, 0.95, (0, 0, 0)),
    "headlight": ((0.95, 0.95, 0.9, 1.0), 0.0, 0.1, (4.0, 4.0, 3.6)),
    "taillight": ((0.6, 0.02, 0.02, 1.0), 0.0, 0.2, (1.6, 0.05, 0.03)),
}


class Mesh:
    def __init__(self):
        self.parts = {}

    def _p(self, mat):
        return self.parts.setdefault(mat, ([], [], []))

    def tri(self, mat, a, b, c, n=None):
        pos, nrm, idx = self._p(mat)
        g = cross(sub(b, a), sub(c, a))
        if n is None:
            n = norm(g)
        elif g[0] * n[0] + g[1] * n[1] + g[2] * n[2] < 0:
            b, c = c, b   # winding always agrees with the (outward) normal: front faces outside
        base = len(pos)
        for p in (a, b, c):
            pos.append(p)
            nrm.append(n)
        idx += [base, base + 1, base + 2]

    def quad(self, mat, a, b, c, d):
        n = norm(cross(sub(b, a), sub(c, a)))
        self.tri(mat, a, b, c, n)
        self.tri(mat, a, c, d, n)

    def hexa(self, mat, c):
        """Closed hexahedron from 8 corners: c[0..3] bottom (ccw seen from below... any order
        consistent with box()), c[4..7] top above them."""
        f = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        cen = mul(add(add(add(c[0], c[1]), add(c[2], c[3])), add(add(c[4], c[5]), add(c[6], c[7]))), 1.0 / 8.0)
        for q in f:
            a, b, cc, d = c[q[0]], c[q[1]], c[q[2]], c[q[3]]
            n = norm(cross(sub(b, a), sub(cc, a)))
            fc = mul(add(add(a, b), add(cc, d)), 0.25)
            out = sub(fc, cen)
            if n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0:
                n = mul(n, -1.0)   # outward, whatever the corner order
            self.tri(mat, a, b, cc, n)
            self.tri(mat, a, cc, d, n)

    def box(self, mat, center, half):
        cx, cy, cz = center
        hx, hy, hz = half
        c = [(cx - hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz + hz), (cx - hx, cy - hy, cz + hz),
             (cx - hx, cy + hy, cz - hz), (cx + hx, cy + hy, cz - hz), (cx + hx, cy + hy, cz + hz), (cx - hx, cy + hy, cz + hz)]
        self.hexa(mat, c)

    def slab(self, mat, a, b, c, d, t):
        """A thin plate on the quad a b c d (thickness t, centred): visible from both sides."""
        n = norm(cross(sub(b, a), sub(d, a)))
        o = mul(n, t * 0.5)
        lo = [sub(p, o) for p in (a, b, c, d)]
        hi = [add(p, o) for p in (a, b, c, d)]
        self.hexa(mat, lo + hi)

    def cylinder_x(self, mat, center, radius, half_w, seg=24, inner=0.0):
        """Cylinder (or tube when inner > 0) along X."""
        cx, cy, cz = center
        for i in range(seg):
            a0 = 2 * math.pi * i / seg
            a1 = 2 * math.pi * (i + 1) / seg
            p = lambda r, a, x: (x, cy + r * math.cos(a), cz + r * math.sin(a))
            # tread
            n0 = (0, math.cos((a0 + a1) / 2), math.sin((a0 + a1) / 2))
            self.tri(mat, p(radius, a0, cx - half_w), p(radius, a1, cx - half_w), p(radius, a1, cx + half_w), n0)
            self.tri(mat, p(radius, a0, cx - half_w), p(radius, a1, cx + half_w), p(radius, a0, cx + half_w), n0)
            for s, x in ((-1, cx - half_w), (1, cx + half_w)):
                sn = (float(s), 0.0, 0.0)   # caps face out along the axle
                if inner > 0:
                    a, b, c, d = p(inner, a0, x), p(radius, a0, x), p(radius, a1, x), p(inner, a1, x)
                    self.tri(mat, a, b, c, sn)
                    self.tri(mat, a, c, d, sn)
                else:
                    a, b = p(radius, a0, x), p(radius, a1, x)
                    self.tri(mat, (x, cy, cz), a, b, sn)

    def bar(self, mat, a, b, r):
        """A square bar from a to b (half width r)."""
        d = norm(sub(b, a))
        up = (0, 1, 0) if abs(d[1]) < 0.9 else (1, 0, 0)
        x = mul(norm(cross(d, up)), r)
        y = mul(norm(cross(x, d)), r)
        c = [add(add(a, mul(x, sx)), mul(y, sy)) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        e = [add(add(b, mul(x, sx)), mul(y, sy)) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        self.hexa(mat, c + e)


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    l = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)


def write_gltf(name, mesh):
    buf = bytearray()
    accessors, views, meshes, nodes, materials = [], [], [], [], []
    mat_index = {}
    for mat, (pos, nrm, idx) in mesh.parts.items():
        if not idx:
            continue
        if mat not in mat_index:
            col, metal, rough, emis = MATS[mat]
            m = {"name": mat, "pbrMetallicRoughness": {"baseColorFactor": list(col), "metallicFactor": metal, "roughnessFactor": rough}}
            if any(emis):
                m["emissiveFactor"] = [min(e, 1.0) for e in emis]
                m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": max(emis)}}
            mat_index[mat] = len(materials)
            materials.append(m)
        prims = []
        for data, kind, comp, count, typ, target in (
            (b"".join(struct.pack("<3f", *p) for p in pos), "pos", 5126, len(pos), "VEC3", 34962),
            (b"".join(struct.pack("<3f", *n) for n in nrm), "nrm", 5126, len(nrm), "VEC3", 34962),
            (b"".join(struct.pack("<I", i) for i in idx), "idx", 5125, len(idx), "SCALAR", 34963),
        ):
            while len(buf) % 4:
                buf.append(0)
            views.append({"buffer": 0, "byteOffset": len(buf), "byteLength": len(data), "target": target})
            buf += data
            acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": typ}
            if kind == "pos":
                acc["min"] = [min(p[k] for p in pos) for k in range(3)]
                acc["max"] = [max(p[k] for p in pos) for k in range(3)]
            accessors.append(acc)
            prims.append(len(accessors) - 1)
        meshes.append({"name": mat, "primitives": [{"attributes": {"POSITION": prims[0], "NORMAL": prims[1]}, "indices": prims[2], "material": mat_index[mat]}]})
        nodes.append({"name": mat, "mesh": len(meshes) - 1})
    doc = {
        "asset": {"version": "2.0", "generator": "tools/gen_vehicle_mesh.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buf)).decode()}],
    }
    os.makedirs(ROOT, exist_ok=True)
    with open(os.path.join(ROOT, name + ".gltf"), "w", newline="\n") as f:
        json.dump(doc, f, separators=(",", ":"))
    tris = sum(len(p[2]) for p in mesh.parts.values()) // 3
    print("%s.gltf: %d triangles, %d materials" % (name, tris, len(materials)))


# ----------------------------------------------------------------------------- car

def car_body():
    m = Mesh()
    W, L = 0.86, 2.05          # half width / half length of the lower body
    yb, yt = 0.34, 0.92        # lower body bottom / beltline
    cw = 0.78                  # cabin half width
    roof = 1.44
    zf, zr = -0.95, 1.38       # cabin front (windshield base) / rear (hatch base)
    zwf, zrr = -0.40, 1.15     # roof front / back
    t = 0.04
    # Lower body shell: sides, nose, tail, underbody, hood, hatch deck.
    for s in (-1, 1):
        x = s * W
        m.slab("paint", (x, yb, -L + 0.15), (x, yb, L - 0.1), (x, yt, L - 0.1), (x, yt, -L + 0.15), t)
        # wheel arch lips (dark)
        for z in (-1.30, 1.25):
            m.box("plastic", (x + s * 0.01, 0.56, z), (0.03, 0.05, 0.42))
    m.slab("paint", (-W, yb, -L), (W, yb, -L), (W, yt - 0.06, -L), (-W, yt - 0.06, -L), t)                      # nose
    m.slab("paint", (-W, yt - 0.06, -L), (W, yt - 0.06, -L), (W, yt, -L + 0.15), (-W, yt, -L + 0.15), t)
    m.slab("paint", (-W, yb, L), (-W, yt, L - 0.05), (W, yt, L - 0.05), (W, yb, L), t)                          # tail
    m.slab("paint_dark", (-W, yb, -L), (-W, yb, L), (W, yb, L), (W, yb, -L), t)                                 # underbody
    m.slab("paint", (-W, yt, -L + 0.15), (-cw, yt, zf), (cw, yt, zf), (W, yt, -L + 0.15), t)                    # hood (to the windshield)
    m.slab("paint", (-W, yt, L - 0.05), (W, yt, L - 0.05), (cw, yt, zr), (-cw, yt, zr), t)                      # hatch deck
    # Cabin: roof, pillars, glass, door tops.
    m.slab("paint", (-cw + 0.04, roof, zwf), (cw - 0.04, roof, zwf), (cw - 0.04, roof, zrr), (-cw + 0.04, roof, zrr), t)
    for s in (-1, 1):
        x = s * cw
        m.bar("paint", (x, yt, zf), (x - s * 0.04, roof, zwf), 0.035)          # A pillar
        m.bar("paint", (x, yt, 0.42), (x - s * 0.04, roof, 0.42), 0.04)        # B pillar
        m.bar("paint", (x, yt, zr), (x - s * 0.04, roof, zrr), 0.05)           # C pillar
        m.slab("glass", (x, yt + 0.02, zf + 0.12), (x, yt + 0.02, 0.38), (x - s * 0.04, roof - 0.03, 0.38), (x - s * 0.04, roof - 0.03, zwf + 0.06), 0.01)
        m.slab("glass", (x, yt + 0.02, 0.46), (x, yt + 0.02, zr - 0.08), (x - s * 0.04, roof - 0.03, zrr - 0.05), (x - s * 0.04, roof - 0.03, 0.46), 0.01)
        m.box("chrome", (x + s * 0.02, 0.78, -0.15), (0.015, 0.012, 0.07))      # door handles
        m.box("chrome", (x + s * 0.02, 0.78, 0.75), (0.015, 0.012, 0.07))
        m.box("plastic", (x + s * 0.09, 1.0, zf + 0.15), (0.07, 0.05, 0.03))   # mirrors
    m.slab("glass", (-cw + 0.03, yt + 0.01, zf), (cw - 0.03, yt + 0.01, zf), (cw - 0.07, roof - 0.01, zwf), (-cw + 0.07, roof - 0.01, zwf), 0.012)   # windshield
    m.slab("glass", (-cw + 0.06, yt + 0.03, zr), (-cw + 0.07, roof - 0.02, zrr), (cw - 0.07, roof - 0.02, zrr), (cw - 0.06, yt + 0.03, zr), 0.012)   # rear window
    # Bumpers, grille, plates, lights.
    m.box("plastic", (0, 0.42, -L - 0.03), (W + 0.02, 0.09, 0.06))
    m.box("plastic", (0, 0.42, L + 0.03), (W + 0.02, 0.09, 0.06))
    m.box("plastic", (0, 0.66, -L - 0.005), (0.42, 0.07, 0.01))
    for s in (-1, 1):
        m.box("headlight", (s * 0.6, 0.72, -L - 0.005), (0.17, 0.06, 0.012))
        m.box("taillight", (s * 0.66, 0.78, L + 0.005), (0.13, 0.07, 0.012))
    m.box("plastic", (-0.5, 0.3, L + 0.02), (0.035, 0.035, 0.08))               # exhaust tip
    # Interior: floor, firewall / dash, seats.
    m.slab("plastic", (-W + 0.05, 0.42, -0.9), (W - 0.05, 0.42, -0.9), (W - 0.05, 0.42, 1.35), (-W + 0.05, 0.42, 1.35), 0.03)
    m.box("plastic", (0, 0.86, -0.78), (cw - 0.04, 0.12, 0.17))                 # dashboard
    m.box("plastic", (-0.36, 0.95, -0.66), (0.14, 0.04, 0.06))                  # instrument binnacle
    m.box("plastic", (0, 0.62, -0.25), (0.1, 0.12, 0.45))                       # centre console
    for x, z in ((-0.36, 0.05), (0.36, 0.05), (-0.38, 0.95), (0.38, 0.95)):
        m.box("fabric", (x, 0.58, z - 0.02), (0.24, 0.07, 0.25))                # cushion
        m.box("fabric", (x, 0.95, z + 0.24), (0.24, 0.32, 0.06))                # backrest
        m.box("fabric", (x, 1.3, z + 0.25), (0.12, 0.07, 0.05))                 # headrest
    write_gltf("car_body", m)


def wheel(name, radius, half_w, spokes):
    m = Mesh()
    m.cylinder_x("rubber", (0, 0, 0), radius, half_w, 28, inner=radius * 0.64)
    m.cylinder_x("metal", (0, 0, 0), radius * 0.66, half_w * 0.8, 20)
    for i in range(spokes):
        a = 2 * math.pi * i / spokes
        p = (half_w * 0.85, radius * 0.6 * math.cos(a), radius * 0.6 * math.sin(a))
        m.bar("chrome", (half_w * 0.85, 0, 0), p, 0.012 if spokes > 8 else 0.025)
        m.bar("chrome", (-half_w * 0.85, 0, 0), (-p[0], p[1], p[2]), 0.012 if spokes > 8 else 0.025)
    m.box("chrome", (0, 0, 0), (half_w * 0.95, radius * 0.12, radius * 0.12))
    write_gltf(name, m)


def car_steering():
    m = Mesh()
    r, seg = 0.19, 20
    for i in range(seg):
        a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
        m.bar("plastic", (r * math.cos(a0), 0, r * math.sin(a0)), (r * math.cos(a1), 0, r * math.sin(a1)), 0.016)
    for a in (0.0, math.pi, math.pi * 0.5):
        m.bar("plastic", (0, -0.02, 0), (r * math.cos(a), 0, r * math.sin(a)), 0.012)
    m.box("plastic", (0, -0.02, 0), (0.05, 0.03, 0.05))
    m.bar("plastic", (0, -0.03, 0), (0, -0.3, 0), 0.025)                         # column
    write_gltf("car_steering", m)


# ----------------------------------------------------------------------------- motorcycle

def moto_body():
    m = Mesh()
    m.bar("metal", (0, 0.5, -0.5), (0, 0.95, 0.35), 0.035)                       # main tube
    m.bar("metal", (0, 0.42, -0.35), (0, 0.4, 0.45), 0.03)                       # lower tube
    m.bar("metal", (0, 1.02, -0.52), (0, 0.5, -0.5), 0.035)                      # head tube -> down tube
    for s in (-1, 1):
        m.bar("metal", (s * 0.08, 0.45, 0.2), (s * 0.08, 0.31, 0.72), 0.022)    # swing arm
        m.bar("metal", (s * 0.07, 0.86, 0.45), (s * 0.08, 0.31, 0.72), 0.018)   # shocks
        m.box("rubber", (s * 0.2, 0.38, 0.05), (0.05, 0.015, 0.035))             # pegs
        m.box("rubber", (s * 0.18, 0.43, 0.5), (0.04, 0.012, 0.03))              # passenger pegs
    m.box("metal", (0, 0.52, 0.0), (0.13, 0.13, 0.2))                            # engine
    m.box("paint", (0, 1.0, -0.2), (0.16, 0.09, 0.26))                           # tank
    m.box("fabric", (0, 0.96, 0.32), (0.15, 0.04, 0.32))                         # seat
    m.box("paint", (0, 0.92, 0.7), (0.12, 0.04, 0.12))                           # tail
    m.box("taillight", (0, 0.92, 0.83), (0.06, 0.025, 0.01))
    m.bar("chrome", (0.16, 0.42, 0.1), (0.2, 0.42, 0.95), 0.035)                 # exhaust
    m.box("paint", (0, 0.68, -0.74), (0.08, 0.025, 0.2))                         # front fender
    m.box("paint", (0, 0.72, 0.76), (0.09, 0.025, 0.18))                         # rear fender
    write_gltf("moto_body", m)


def moto_bar():
    # Local frame: +Y = steering axis (pivot at the origin), bar along X; the fork runs down -Y
    # to the front axle (0.82 m along the axis).
    m = Mesh()
    m.bar("metal", (-0.28, 0.02, 0.04), (0.28, 0.02, 0.04), 0.012)
    for s in (-1, 1):
        m.box("rubber", (s * 0.3, 0.02, 0.04), (0.045, 0.018, 0.018))           # grips
        m.bar("chrome", (s * 0.085, 0.0, 0.0), (s * 0.085, -0.8, 0.0), 0.022)   # fork tubes
        m.box("plastic", (s * 0.2, 0.12, -0.02), (0.035, 0.015, 0.025))         # mirrors
        m.bar("plastic", (s * 0.18, 0.03, 0.03), (s * 0.2, 0.11, -0.02), 0.006)
    m.box("metal", (0, 0.0, 0.0), (0.1, 0.03, 0.04))                            # triple clamp
    m.box("headlight", (0, -0.12, -0.09), (0.08, 0.07, 0.03))
    m.box("plastic", (0, 0.06, -0.02), (0.06, 0.03, 0.04))                      # clocks
    write_gltf("moto_bar", m)


if __name__ == "__main__":
    car_body()
    wheel("car_wheel", 0.32, 0.11, 5)
    car_steering()
    moto_body()
    wheel("moto_wheel", 0.31, 0.065, 18)
    moto_bar()
