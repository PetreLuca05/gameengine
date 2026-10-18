"""Chest rig (data/items.json "rig_chest"): an olive nylon placard: four rifle magazine pouches (shock-cord
retention with pull tabs) above two flapped utility pouches (side-release buckles), MOLLE webbing
rows, the H-harness shoulder straps (laid flat, behind the placard) and the side straps with buckles.
Carried by the drag loop on the placard top (grip_r at the origin). Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("rig_chest", "item")
    w, h, t = 0.26, 0.2, 0.03
    top = -0.03   # placard top below the hand
    cy = top - h * 0.5
    panel = L.box((0, cy, 0), (w * 0.5, h * 0.5, t * 0.5), 0.008, 3, "fabric_nylon")
    binding = L.box((0, cy, -0.0005), (w * 0.5 + 0.002, h * 0.5 + 0.002, t * 0.5 - 0.004), 0.004, 2, "webbing_black")
    # Mag pouches: open-top boxes with a shock cord loop and a pull tab.
    pouches = []
    for k in range(4):
        x = -w * 0.5 + 0.034 + k * 0.064
        p = L.box((x, top - 0.05, t * 0.5 + 0.017), (0.028, 0.048, 0.016), 0.005, 3, "fabric_nylon")
        p.cut(L.box((x, top + 0.002, t * 0.5 + 0.017), (0.024, 0.01, 0.012), 0, 0))
        mag = L.box((x, top - 0.022, t * 0.5 + 0.017), (0.0115, 0.034, 0.0105), 0.0015, 2, "polymer_tan")
        cord = L.box((x, top + 0.004, t * 0.5 + 0.034), (0.024, 0.0018, 0.0018), 0.0006, 1, "webbing_black")
        tab = L.box((x, top + 0.012, t * 0.5 + 0.034), (0.008, 0.008, 0.0014), 0.0008, 2, "webbing_black")
        pouches += [p, mag, cord, tab]
    # Utility pouches with flaps and buckles.
    util = []
    for sx in (1, -1):
        x = sx * 0.064
        util.append(L.box((x, top - h + 0.045, t * 0.5 + 0.02), (0.058, 0.04, 0.02), 0.007, 3, "fabric_nylon"))
        util.append(L.box((x, top - h + 0.082, t * 0.5 + 0.024), (0.06, 0.012, 0.022), 0.006, 3, "fabric_nylon"))
        util.append(L.box((x, top - h + 0.062, t * 0.5 + 0.046), (0.011, 0.016, 0.004), 0.0015, 2, "plastic_black"))
    molle = [L.box((0, top - h + 0.012 + k * 0.024, -t * 0.5 - 0.0015), (w * 0.47, 0.007, 0.0015), 0.0008, 1, "webbing_black") for k in range(3)]
    # H harness: two shoulder straps over the placard top, hanging down behind it (laid flat), joined by the
    # back strap; the drag loop on the placard top is the hand (grip_r).
    harness = []
    for sx in (1, -1):
        s = L.side_prism([(-t * 0.5 + 0.004, top - 0.012), (-t * 0.5 - 0.004, top + 0.02), (-0.05, top + 0.02), (-0.058, top - 0.15), (-0.046, top - 0.15),
                          (-0.04, top + 0.008), (-t * 0.5 - 0.002, top + 0.006), (-t * 0.5 - 0.004, top - 0.012)], sx * 0.09 - 0.022, sx * 0.09 + 0.022,
                         bevel=0.003, segs=2, mat="webbing_black", angle=15)
        harness.append(s)
    yoke = L.box((0, top - 0.12, -0.054), (0.112, 0.012, 0.004), 0.002, 2, "webbing_black")
    loop = L.torus((0, top + 0.016, 0), (0, 0, 1), 0.016, 0.004, 24, 8, "webbing_black")
    loop.scale((1.3, 1.0, 0.6), (0, top + 0.016, 0))
    sides = []
    for sx in (1, -1):
        sides.append(L.box((sx * (w * 0.5 + 0.04), top - h + 0.03, -0.01), (0.04, 0.012, 0.002), 0.0012, 2, "webbing_black"))
        sides.append(L.box((sx * (w * 0.5 + 0.08), top - h + 0.03, -0.01), (0.012, 0.016, 0.006), 0.002, 2, "plastic_black"))
    label = L.text("DG", (w * 0.5 - 0.02, top - 0.012, t * 0.5 + 0.0004), 0.008, (0, 0, 1), (0, 1, 0), 0.0002, "label_white")
    it.part("placard").add(panel, binding, *molle, label)
    it.part("pouches").add(*pouches, *util)
    it.part("harness", mech="straps").add(*harness, yoke, *sides)
    it.part("drag_loop").add(loop)
    it.socket("grip_r", (0, 0, 0))
    it.socket("chest", (0, cy, -t * 0.5))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    return it
