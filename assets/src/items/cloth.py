"""Cloth (data/items.json "cloth"): a folded stack of rags: three folded layers of canvas / cotton with
rounded folds and frayed edges, tied with a strip. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("cloth", "item")
    layers = []
    mats = ("fabric_canvas", "fabric_blue", "fabric_gauze")
    for k in range(3):
        y = k * 0.0085
        lay = L.box((0.002 * k, y + 0.004, -0.003 * k), (0.11 - 0.006 * k, 0.004, 0.085 - 0.004 * k), 0.0038, 4, mats[k])
        for v in lay.bm.verts:
            v.co.y += 0.0012 * ((v.co.x * 91.0 + v.co.z * 57.0) % 1.0 - 0.5)
        layers.append(lay)
    tie = L.box((0.02, 0.012, 0.0), (0.009, 0.0145, 0.088), 0.004, 3, "fabric_canvas")
    tie.cut(L.box((0.02, 0.012, 0.0), (0.012, 0.0122, 0.083), 0.003, 2))
    knot = L.sphere((0.02, 0.027, 0.03), 0.008, 16, 8, "fabric_canvas")
    it.part("stack").add(*layers, tie, knot)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
