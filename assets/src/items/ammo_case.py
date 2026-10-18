"""Ammo case (data/items.json "ammo_case"): a steel ammunition can in olive paint: the pressed body with
its stiffening beads and stencilled markings, the lid on its hinge (separate part) with the cam latch
lever and the rubber gasket, the folding wire carry handle on top. Real size 19 x 14 x 9 cm,
palm-held. Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("ammo_case", "item")
    w, h, d = 0.19, 0.14, 0.09
    body = L.box((0, h * 0.44, 0), (w * 0.5, h * 0.44, d * 0.5), 0.006, 3, "paint_olive")
    beads = [L.box((0, y, sz * (d * 0.5 + 0.0007)), (w * 0.47, 0.003, 0.0012), 0.0008, 2, "paint_olive") for y in (h * 0.2, h * 0.68) for sz in (1, -1)]
    lid = L.box((0, h * 0.94, 0), (w * 0.5 + 0.002, h * 0.06, d * 0.5 + 0.002), 0.004, 3, "paint_olive")
    lid_lip = L.box((0, h * 0.885, 0), (w * 0.5 + 0.003, 0.004, d * 0.5 + 0.003), 0.0015, 2, "paint_olive")
    gasket = L.box((0, h * 0.88, 0), (w * 0.5 - 0.004, 0.0015, d * 0.5 - 0.004), 0.0006, 1, "rubber")
    hinge = L.cyl((-w * 0.5 + 0.01, h * 0.88, -d * 0.5 - 0.004), (w * 0.5 - 0.01, h * 0.88, -d * 0.5 - 0.004), 0.003, 16, "steel_dark", bevel=0.0005)
    lever = L.side_prism([(d * 0.5 + 0.003, h * 0.98), (d * 0.5 + 0.009, h * 0.95), (d * 0.5 + 0.009, h * 0.62), (d * 0.5 + 0.004, h * 0.6), (d * 0.5 + 0.003, h * 0.62)],
                         w * 0.5 - 0.035, w * 0.5 - 0.005, bevel=0.0012, segs=2, mat="paint_olive")
    lever_pin = L.cyl((w * 0.5 - 0.038, h * 0.96, d * 0.5 + 0.005), (w * 0.5 - 0.002, h * 0.96, d * 0.5 + 0.005), 0.0022, 12, "steel_dark", bevel=0.0004)
    # Wire handle folded flat on the lid, in its two pivot brackets.
    wire = L.torus((0, h + 0.002, 0), (0, 1, 0), 0.05, 0.0028, 48, 8, "steel_dark")
    wire.scale((1.0, 1.0, 0.25), (0, h + 0.002, 0))
    wire.cut(L.box((0, h + 0.002, -0.05), (0.1, 0.02, 0.05), 0, 0))
    grip = L.cyl((-0.035, h + 0.002, 0.0125), (0.035, h + 0.002, 0.0125), 0.006, 20, "plastic_black", bevel=0.001)
    brackets = [L.box((sx * 0.05, h + 0.003, 0), (0.004, 0.004, 0.006), 0.001, 2, "steel_dark") for sx in (1, -1)]
    marks = [L.text("7.62MM  420 CTG", (0, h * 0.44, d * 0.5 + 0.0016), 0.012, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow"),
             L.text("DG AMMO  M19A1", (0, h * 0.3, d * 0.5 + 0.0016), 0.007, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow")]
    it.part("can").add(body, *beads, hinge, *marks)
    it.part("lid", pivot=(0, h * 0.88, -d * 0.5 - 0.004), mech="lid").add(lid, lid_lip, gasket, wire, grip, *brackets)
    it.part("latch", pivot=(w * 0.5 - 0.02, h * 0.96, d * 0.5 + 0.005), mech="latch").add(lever, lever_pin)
    it.socket("grip_r", (0, 0, 0))
    it.socket("lid", (0, h * 0.88, 0))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    it.hold_small()
    return it
