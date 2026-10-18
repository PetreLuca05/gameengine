"""Mechanical parts (data/items.json "mech_parts"): a cluster of salvage hardware: a spur gear, a
compression spring, a bearing, bolts and nuts. Palm-held. Procedural (path b)."""
import math

import item_lib as L


def build(ctx):
    it = L.Item("mech_parts", "item")
    teeth = 18
    pts = []
    for k in range(teeth):
        a0 = 2 * math.pi * k / teeth
        for (da, r) in ((0.0, 0.026), (0.06, 0.032), (0.18, 0.032), (0.24, 0.026)):
            a = a0 + da * 2 * math.pi / teeth / 0.3
            pts.append((r * math.cos(a), r * math.sin(a)))
    gear = L.top_prism(pts, 0.0, 0.008, bevel=0.0005, segs=1, mat="steel_bare")
    gear.cut(L.cyl((0, -0.01, 0), (0, 0.02, 0), 0.006, 24, bevel=0))
    gear.cut(L.merge(*[L.cyl((math.cos(k * 1.5708) * 0.016, -0.01, math.sin(k * 1.5708) * 0.016), (math.cos(k * 1.5708) * 0.016, 0.02, math.sin(k * 1.5708) * 0.016), 0.0045, 16,
                             bevel=0) for k in range(4)]))
    spring = []
    for k in range(40):
        t = k / 40.0
        a = t * 2 * math.pi * 6
        spring.append(L.sphere((0.045 + 0.008 * math.cos(a), 0.004 + t * 0.045, -0.02 + 0.008 * math.sin(a)), 0.0016, 8, 4, "steel_dark"))
    bearing = L.ring_lathe([(0.005, 0), (0.011, 0), (0.011, 0.007), (0.005, 0.007)], (-0.04, 0.0, -0.025), (0, 1, 0), 40, "steel_stainless", bevel=0.0004)
    inner = L.ring_lathe([(0.004, 0), (0.0065, 0), (0.0065, 0.007), (0.004, 0.007)], (-0.04, 0.0, -0.025), (0, 1, 0), 32, "steel_bare")
    bolts = []
    for (x, z, a) in ((-0.03, 0.03, 20), (0.0, 0.045, -40)):
        b = L.cyl((x, 0.0045, z), (x + 0.04 * math.cos(math.radians(a)), 0.0045, z + 0.04 * math.sin(math.radians(a))), 0.0035, 16, "steel_bare", bevel=0.0004)
        bolts.append(b)
        bolts.append(L.ring_lathe([(0.0, 0), (0.0062, 0), (0.0062, 0.004), (0.0, 0.004)], (x, 0.0045, z), (-math.cos(math.radians(a)), 0, -math.sin(math.radians(a))), 6, "steel_bare"))
    it.part("parts").add(gear, bearing, inner, *spring, *bolts)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
