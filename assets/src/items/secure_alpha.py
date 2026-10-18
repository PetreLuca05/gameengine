"""Secure container (data/items.json "secure_alpha"): a small dark grey hard case: ribbed polymer shell
halves with the gasket line, two draw latches (separate part), the folding carry handle, the hinge
knuckles at the back and an embossed "SECURE" plate. Real size 16 x 9 x 16 cm, palm-held.
Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("secure_alpha", "item")
    w, h, d = 0.16, 0.09, 0.16
    base = L.box((0, h * 0.3, 0), (w * 0.5, h * 0.3, d * 0.5), 0.012, 4, "polymer_matte")
    lid = L.box((0, h * 0.8, 0), (w * 0.5, h * 0.2, d * 0.5), 0.012, 4, "polymer_matte")
    gasket = L.box((0, h * 0.6, 0), (w * 0.5 + 0.0012, 0.002, d * 0.5 + 0.0012), 0.0015, 2, "rubber")
    ribs = [L.box((x, h * 0.3, d * 0.5 + 0.002), (0.003, h * 0.24, 0.002), 0.0012, 2, "polymer_matte") for x in (-0.06, 0.06)]
    ribs += [L.box((x, h * 0.3, -d * 0.5 - 0.002), (0.003, h * 0.24, 0.002), 0.0012, 2, "polymer_matte") for x in (-0.06, 0.06)]
    lid_ribs = [L.box((0, h, z), (w * 0.44, 0.0025, 0.004), 0.0012, 2, "polymer_matte") for z in (-0.05, 0.0, 0.05)]
    plate = L.box((0, h * 0.3, d * 0.5 + 0.0018), (0.035, 0.012, 0.0012), 0.001, 2, "steel_stainless")
    text = L.text("SECURE", (0, h * 0.3, d * 0.5 + 0.0032), 0.009, (0, 0, 1), (0, 1, 0), 0.0002, "engrave")
    knuckles = [L.cyl((x - 0.015, h * 0.6, -d * 0.5 - 0.004), (x + 0.015, h * 0.6, -d * 0.5 - 0.004), 0.005, 16, "polymer_matte", bevel=0.0008) for x in (-0.06, 0.06)]
    latches = []
    for x in (-0.06, 0.06):
        latches.append(L.box((x, h * 0.6, d * 0.5 + 0.006), (0.012, 0.018, 0.004), 0.0015, 2, "steel_stainless"))
        latches.append(L.cyl((x - 0.012, h * 0.6 - 0.012, d * 0.5 + 0.005), (x + 0.012, h * 0.6 - 0.012, d * 0.5 + 0.005), 0.0022, 12, "steel_stainless", bevel=0.0004))
    handle = L.side_prism([(-0.012, h + 0.004), (-0.008, h + 0.016), (0.008, h + 0.016), (0.012, h + 0.004), (0.006, h + 0.004), (0.004, h + 0.01), (-0.004, h + 0.01),
                           (-0.006, h + 0.004)], -0.045, 0.045, bevel=0.0012, segs=2, mat="rubber", angle=15)
    handle.rotate((0, 1, 0), 90, (0, h, 0))
    it.part("case").add(base, gasket, *ribs, plate, text, *knuckles)
    it.part("lid", pivot=(0, h * 0.6, -d * 0.5 - 0.004), mech="lid").add(lid, *lid_ribs, handle)
    it.part("latches", mech="latch").add(*latches)
    it.socket("grip_r", (0, 0, 0))
    it.socket("lid", (0, h * 0.6, 0))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    it.hold_small()
    return it
