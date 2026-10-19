"""Lights / lasers (attachment space: origin = the underbarrel rail socket, the device hangs below it, -Z
forward) and the handheld flashlight."""
import math

import _optics as O
import item_lib as L


def hanging_clamp(z0, z1, mat="aluminium_anodized"):
    """A rail clamp for a device below the rail: the optic clamp turned upside down."""
    parts = O.rail_clamp(z0, z1, mat, knob_side=1)
    for p in parts:
        p.rotate((0, 0, 1), 180)
    return parts


def head(z, r, y, depth=0.024, mat="aluminium_anodized", crenel=True):
    """Bezel + reflector + lens at z (facing -Z): returns solids and the emitter point."""
    bez = L.lathe([(r - 0.0035, 0), (r, 0), (r, depth - 0.004), (r - 0.0008, depth), (r - 0.003, depth)], (0, y, z + depth), (0, 0, -1), 56, mat, bevel=0.0004)
    bez.cut(L.cyl((0, y, z - 0.002), (0, y, z + 0.006), r - 0.003, 48, bevel=0))
    if crenel:
        for k in range(6):
            a = k * math.pi / 3
            bez.cut(L.box((math.cos(a) * r, y + math.sin(a) * r, z + 0.0008), (0.0028, 0.0028, 0.0012), 0, 0).rotate((0, 0, 1), math.degrees(a), (0, y, z)))
    refl = L.lathe([(0.0016, 0.0), (r - 0.0035, 0.009), (r - 0.0032, 0.0094), (0.0016, 0.0006)], (0, y, z + 0.0115), (0, 0, -1), 48, "aluminium_bare")
    led = L.box((0, y, z + 0.0115), (0.0016, 0.0016, 0.0008), 0.0003, 1, "emitter")
    lens = O.lens((0, y, z + 0.0018), (0, 0, -1), r - 0.0032, "glass_clear", 0.0004)
    return [bez, refl, led, lens], (0, y, z)


def knurled_tube(z0, z1, y, r, mat="aluminium_anodized", bands=(), rings=0):
    t = L.cyl((0, y, z0), (0, y, z1), r, 48, mat, bevel=0.0006)
    for (a, b) in bands:
        t.cut(L.knurl((0, y, a), (0, y, b), r, 36, 0.0004))
    if rings:
        t.cut(L.merge(*[L.torus((0, y, z0 + (z1 - z0) * (k + 1) / (rings + 1)), (0, 0, 1), r, 0.0005, 48, 6) for k in range(rings)]))
    return t
