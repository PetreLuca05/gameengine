"""Optic / mount sub-assemblies (attachment space: origin = the rail socket the optic mounts on, -Z forward).

Lens rule (docs/ITEM_PIPELINE.md "Optics"): the sight pass draws the window / ocular image at the JSON
lens position, so the mesh keeps the view path clear: no opaque geometry inside the window radius in front
of the eye-side lens plane; the scope ocular lens sits a few millimetres forward of the lens plane."""
import math

import item_lib as L


def rail_clamp(z0, z1, mat="aluminium_anodized", knob_side=-1, height=0.004):
    """Picatinny clamp base from z0..z1 (mount face at y 0): dovetail jaws, recoil lug, cross bolt with a
    thumb knob (or hex nut) on knob_side."""
    za, zb = min(z0, z1), max(z0, z1)
    w = 0.0125
    base = L.front_prism([(-w, 0.0), (-0.0106 + 0.0008, 0.0), (-0.0106, -0.0025), (-0.0128, -0.0035), (-0.0128, -0.0015), (-w, height), (w, height), (0.0128, -0.0015),
                          (0.0128, -0.0035), (0.0106, -0.0025), (0.0106 - 0.0008, 0.0), (w, 0.0)], za, zb, bevel=0.0006, segs=2, mat=mat)
    lug = L.box((0, -0.0025, (za + zb) * 0.5), (0.008, 0.0025, 0.0024), 0.0003, 1, mat)
    bolt = L.cyl((-0.016, -0.0015, (za + zb) * 0.5), (0.016, -0.0015, (za + zb) * 0.5), 0.0022, 16, "steel_dark", bevel=0.0003)
    knob = L.cyl((knob_side * 0.0128, -0.0015, (za + zb) * 0.5), (knob_side * 0.0205, -0.0015, (za + zb) * 0.5), 0.0058, 20, "aluminium_anodized", bevel=0.0006)
    knob.cut(L.knurl((knob_side * 0.013, -0.0015, (za + zb) * 0.5), (knob_side * 0.0205, -0.0015, (za + zb) * 0.5), 0.0058, 20, 0.0004))
    nut = L.lathe([(0, 0), (0.0042, 0), (0.0042, 0.003), (0, 0.003)], (-knob_side * 0.0128, -0.0015, (za + zb) * 0.5), (-knob_side, 0, 0), 6, "steel_dark")
    return [base, lug, bolt, knob, nut]


def scope_ring(z, axis_y, tube_r, base_y=0.004, mat="aluminium_anodized"):
    """Two-piece scope ring around a tube at axis_y: lower half on the base, cap with four screws."""
    w = 0.0065
    outer = tube_r + 0.0032
    lower = L.ring_lathe([(tube_r, -w), (outer, -w), (outer, w), (tube_r, w)], (0, axis_y, z), (0, 0, 1), 48, mat, bevel=0.0005)
    lower.cut(L.box((0, axis_y + outer, z), (outer + 0.004, outer * 0.95, w + 0.002), 0, 0))
    stem = L.box((0, (base_y + axis_y - tube_r) * 0.5, z), (0.0095, (axis_y - tube_r - base_y) * 0.5 + 0.0015, w), 0.0008, 2, mat)
    stem.cut(L.cyl((0, axis_y, z - 0.01), (0, axis_y, z + 0.01), tube_r + 0.0002, 48, bevel=0))
    cap = L.ring_lathe([(tube_r, -w), (outer, -w), (outer, w), (tube_r, w)], (0, axis_y, z), (0, 0, 1), 48, mat, bevel=0.0005)
    cap.cut(L.box((0, axis_y - outer * 0.5, z), (outer + 0.004, outer * 0.5 + 0.0006, w + 0.002), 0, 0))
    screws = [L.screw((sx * (outer + 0.0002), axis_y + 0.0006, z + dz), (sx, 0, 0), 0.0016, 0.0006, slot=False, hexsock=True) for sx in (1, -1) for dz in (-0.0032, 0.0032)]
    return [lower, stem, cap] + screws


def turret(center, axis, r=0.0085, h=0.012, mat="aluminium_anodized", marks=12, cap=True):
    """Adjustment turret on a saddle: base ring, knurled cap with index marks, zero-stop line."""
    from mathutils import Vector
    a = Vector(axis).normalized()
    c = Vector(center)
    base = L.lathe([(0, 0), (r * 1.08, 0), (r * 1.08, h * 0.35), (r * 0.95, h * 0.4), (0, h * 0.4)], c, a, 40, mat)
    knob = L.lathe([(0, 0), (r, 0), (r, h * 0.92), (r * 0.9, h), (0, h)], c + a * (h * 0.38), a, 40, mat)
    knob.cut(L.knurl(c + a * (h * 0.5), c + a * (h * 1.24), r, 32, 0.0004))
    out = [base, knob]
    if marks:
        _, u, w = L._frame(a)
        for k in range(marks):
            ang = 2 * math.pi * k / marks
            d = u * math.cos(ang) + w * math.sin(ang)
            p = c + a * (h * 0.33) + d * (r * 1.08)
            out.append(L.cyl(p, p + d * 0.0004, 0.00045 if k % 3 else 0.0007, 6, "paint_white", bevel=0))
    return out


def lens(center, axis, r, mat="glass_lens", bulge=0.0015, segs=40):
    """A lens element: a slightly convex disc (both faces), r = clear aperture."""
    from mathutils import Vector
    a = Vector(axis).normalized()
    c = Vector(center)
    prof = [(0.0, -bulge), (r * 0.55, -bulge * 0.7), (r, -0.0004), (r, 0.0004), (r * 0.55, bulge * 0.7), (0.0, bulge)]
    return L.lathe(prof, c, a, segs, mat)


def flip_cap(center, axis, r, hinge_side=(0, 1, 0), open_deg=100, mat="polymer_matte", lid=True):
    """Flip-up lens cap, open: a ring on the tube end, the lid swung about a hinge on hinge_side. lid False:
    the ring only (an eye-side cap taken off: open, its lid stands within a few cm of the eye and fills the
    view beside the window wherever it is hinged)."""
    from mathutils import Vector
    a = Vector(axis).normalized()
    c = Vector(center)
    ring = L.ring_lathe([(r + 0.0005, 0), (r + 0.003, 0), (r + 0.003, 0.008), (r + 0.0005, 0.008)], c - a * 0.004, a, 40, mat, bevel=0.0004)
    if not lid:
        return [ring]
    hs = Vector(hinge_side).normalized()
    hinge = c + hs * (r + 0.004) + a * 0.004
    cap = L.lathe([(0, 0), (r + 0.003, 0), (r + 0.003, 0.004), (r + 0.001, 0.005), (0, 0.005)], c + a * 0.004, a, 40, mat)
    tab = L.box(hinge + hs * 0.002, (0.004, 0.004, 0.004), 0.001, 2, mat)
    cap.union(tab)
    # Swing the lid about the hinge axis (perpendicular to the optic axis and the hinge side).
    ax = a.cross(hs).normalized()
    cap.rotate(tuple(ax), open_deg, tuple(hinge))
    return [ring, cap]
