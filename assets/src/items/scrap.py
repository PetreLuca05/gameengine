"""Scrap metal (data/items.json "scrap"): a handful of salvage: a bent sheet-metal bracket with holes, a
length of rusty angle iron, washers, a hex nut and a bolt. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("scrap", "item")
    br = L.top_prism([(-0.06, -0.025), (0.05, -0.03), (0.06, 0.02), (-0.045, 0.032)], 0.0, 0.0018, bevel=0.0004, segs=1, mat="steel_bare")
    for v in br.bm.verts:
        if v.co.x > 0.01:
            v.co.y += (v.co.x - 0.01) * 0.7
    br.cut(L.merge(*[L.cyl((x, -0.02, z), (x, 0.06, z), 0.0042, 16, bevel=0) for (x, z) in ((-0.035, 0.0), (-0.01, 0.005), (0.035, -0.005))]))
    angle = L.front_prism([(0, 0), (0.025, 0), (0.025, 0.003), (0.003, 0.003), (0.003, 0.025), (0, 0.025)], -0.05, 0.05, bevel=0.0005, segs=1, mat="steel_dark")
    angle.rotate((0, 1, 0), 30)
    angle.move((-0.01, 0.002, 0.03))
    washers = [L.ring_lathe([(0.004, 0), (0.0095, 0), (0.0095, 0.0016), (0.004, 0.0016)], (x, 0.0, z), (0, 1, 0), 24, "steel_bare") for (x, z) in ((0.02, -0.04), (0.035, -0.035))]
    nut = L.ring_lathe([(0.0045, 0), (0.0085, 0), (0.0085, 0.006), (0.0045, 0.006)], (-0.04, 0.0, -0.035), (0, 1, 0), 6, "steel_bare")
    bolt = L.cyl((0.0, 0.004, -0.03), (0.05, 0.006, -0.03), 0.0038, 16, "steel_bare", bevel=0.0004)
    bolt.cut(L.merge(*[L.torus((0.012 + k * 0.0016, 0.0045, -0.03), (1, 0, 0), 0.0038, 0.0004, 16, 4) for k in range(20)]))
    head = L.ring_lathe([(0.0, 0), (0.0068, 0), (0.0068, 0.004), (0.0, 0.004)], (0.0, 0.004, -0.03), (-1, 0, 0), 6, "steel_bare")
    it.part("scrap").add(br, angle, *washers, nut, bolt, head)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
