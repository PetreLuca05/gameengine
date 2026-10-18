"""Crossbow bolts (data/items.json "ammo_bolt"): three carbon bolts bundled with two rubber bands,
field points and orange / white vanes. Palm-held along -Z. Procedural (path b)."""
import math

import item_lib as L


def bolt(x, y):
    out = [L.cyl((x, y, 0.18), (x, y, -0.18), 0.0041, 16, "polymer_glossy", bevel=0.0003)]
    out.append(L.lathe([(0, 0), (0.0035, 0), (0.0041, 0.004), (0.0041, 0.011), (0, 0.011)], (x, y, 0.191), (0, 0, -1), 16, "plastic_orange"))
    out.append(L.lathe([(0.0041, 0), (0.0041, 0.006), (0.003, 0.0115), (0, 0.014)], (x, y, -0.18), (0, 0, -1), 16, "steel_bare"))
    for k in range(3):
        a = math.radians(90 + k * 120)
        v = L.side_prism([(0.17, 0.0), (0.11, 0.0), (0.115, 0.009), (0.155, 0.011)], -0.0004, 0.0004, 0.0002, 1, "plastic_white" if k == 0 else "plastic_orange")
        v.move((0, 0.0041, 0))
        v.rotate((0, 0, 1), math.degrees(a) - 90, (0, 0, 0))
        v.move((x, y, 0))
        out.append(v)
    return out


def build(ctx):
    it = L.Item("ammo_bolt", "item")
    parts = []
    for (x, y) in ((-0.0045, 0.0), (0.0045, 0.0), (0.0, 0.0078)):
        parts += bolt(x, y)
    bands = [L.torus((0, 0.0026, z), (0, 0, 1), 0.0098, 0.0012, 24, 6, "rubber") for z in (-0.05, 0.06)]
    it.part("bolts").add(*parts)
    it.part("bands").add(*bands)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
