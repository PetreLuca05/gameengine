"""Combat helmet (data/items.json "helmet"): a high-cut ballistic shell in olive paint (a scaled dome cut
level at the brim, the ear cut-outs), the rubber edge trim, side accessory rails with their bolts, the
front NVG shroud, a rear loop-field patch, the four-point chin strap with its buckle (separate part).
Real size 24 x 18 x 27 cm, palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("helmet", "item")
    rx, ry, rz = 0.115, 0.15, 0.13
    shell = L.sphere((0, 0, 0), 1.0, 48, 24, "paint_olive")
    shell.scale((rx, ry, rz))
    shell.cut(L.box((0, -0.5, 0), (1, 0.5, 1), 0, 0))   # cut at the brim
    inner = L.sphere((0, 0, 0), 1.0, 40, 20, "fabric_nylon")
    inner.scale((rx - 0.008, ry - 0.008, rz - 0.008))
    shell.cut(inner)
    # High cut: the ear openings.
    for sx in (1, -1):
        ear = L.box((sx * rx, 0.0, 0.01), (0.03, 0.045, 0.055), 0.02, 3)
        shell.cut(ear)
    trim = L.torus((0, 0.002, 0), (0, 1, 0), 1.0, 0.0045 / rx, 64, 8, "rubber")   # unit ring, scaled onto the brim
    trim.scale((rx - 0.002, rx - 0.002, rz - 0.002), (0, 0.002, 0))
    rails = []
    for sx in (1, -1):
        r = L.box((sx * 0.1125, 0.05, 0.0), (0.006, 0.012, 0.06), 0.002, 2, "polymer_od")
        r.rotate((1, 0, 0), -8, (sx * rx, 0.05, 0))
        rails.append(r)
        for z in (-0.04, 0.04):
            rails.append(L.cyl((sx * (rx + 0.002), 0.05, z), (sx * (rx + 0.006), 0.05, z), 0.004, 12, "steel_dark", bevel=0.0006))
    shroud = L.box((0, 0.06, -0.122), (0.028, 0.02, 0.006), 0.004, 3, "polymer_matte")
    shroud.rotate((1, 0, 0), 25, (0, 0.06, -0.122))
    shroud_bolts = [L.cyl((x, 0.06, -0.126), (x, 0.06, -0.131), 0.0028, 12, "steel_dark", bevel=0.0004) for x in (-0.018, 0.018)]
    patch = L.box((0, 0.08, 0.112), (0.04, 0.025, 0.004), 0.003, 2, "fabric_nylon")
    patch.rotate((1, 0, 0), -35, (0, 0.08, 0.112))
    strap = L.torus((0, -0.045, 0.01), (1, 0, 0), 0.06, 0.004, 48, 6, "webbing_black")
    strap.scale((1.0, 1.0, 0.7), (0, -0.045, 0.01))
    strap.cut(L.box((0, 0.05, 0), (0.2, 0.06, 0.2), 0, 0))
    straps = [strap.copy().move((sx * (rx - 0.012), 0, 0)) for sx in (1, -1)]
    chin = L.box((0, -0.105, 0.01), (rx - 0.012, 0.004, 0.012), 0.002, 2, "webbing_black")
    buckle = L.box((0.03, -0.105, 0.022), (0.014, 0.008, 0.004), 0.0015, 2, "plastic_black")
    it.part("shell").add(shell, trim, *rails, shroud, *shroud_bolts, patch)
    it.part("chin_strap", mech="straps").add(*straps, chin, buckle)
    it.socket("grip_r", (0, 0, 0))
    it.socket("head", (0, 0.0, 0.0))
    it.hold_small()
    return it
