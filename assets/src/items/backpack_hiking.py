"""Hiking backpack (data/items.json "backpack_hiking", docs/INVENTORY.md: a 30-cell pack): a tall olive
nylon pack: padded body with a drawstring collar under a buckled lid (its own zipped pocket), a front
stretch pocket, two side bottle pockets, side compression straps with ladder-lock buckles, padded
shoulder straps with sternum strap, a padded hip belt with buckle, the top grab handle (grip_r at the
origin) and a name patch. Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("backpack_hiking", "item")
    w, h, d = 0.3, 0.45, 0.18
    top = -0.014
    cy = top - h * 0.5
    body = L.box((0, cy - 0.01, 0), (w * 0.5, h * 0.5 - 0.01, d * 0.5), 0.05, 5, "fabric_nylon")
    collar = L.cyl((0, top - 0.035, 0), (0, top - 0.005, 0), w * 0.36, 40, "fabric_nylon", bevel=0.004)
    collar.scale((1.0, 1.0, (d * 0.42) / (w * 0.36)), (0, top - 0.02, 0))
    lid = L.box((0, top - 0.006, 0.006), (w * 0.47, 0.03, d * 0.52), 0.025, 4, "fabric_nylon")
    lid_zip = L.box((0, top + 0.004, d * 0.5 + 0.018), (w * 0.36, 0.0035, 0.004), 0.0015, 2, "webbing_black")
    front = L.box((0, cy - 0.06, d * 0.5 + 0.022), (w * 0.38, h * 0.3, 0.026), 0.03, 4, "fabric_canvas")
    front_seam = L.box((0, cy + h * 0.24, d * 0.5 + 0.03), (w * 0.37, 0.004, 0.022), 0.002, 2, "webbing_black")
    pockets = [L.box((sx * (w * 0.5 + 0.018), cy - h * 0.26, 0), (0.02, h * 0.17, d * 0.36), 0.014, 3, "fabric_canvas") for sx in (1, -1)]
    # Side compression straps and their ladder locks.
    comp = []
    for sx in (1, -1):
        for yy in (cy + 0.07, cy - 0.06):
            comp.append(L.box((sx * (w * 0.5 + 0.004), yy, 0), (0.0035, 0.011, d * 0.47), 0.0015, 2, "webbing_black"))
            comp.append(L.box((sx * (w * 0.5 + 0.008), yy, d * 0.2), (0.003, 0.014, 0.016), 0.0015, 2, "plastic_black"))
    # Lid straps with buckles down the front.
    lid_straps = []
    for x in (-0.07, 0.07):
        lid_straps.append(L.box((x, top - 0.06, d * 0.5 + 0.05), (0.012, 0.05, 0.003), 0.0012, 2, "webbing_black"))
        lid_straps.append(L.box((x, top - 0.11, d * 0.5 + 0.052), (0.016, 0.012, 0.005), 0.002, 2, "plastic_black"))
    straps = []
    for sx in (1, -1):
        s = L.side_prism([(-d * 0.5 - 0.002, top - 0.03), (-d * 0.5 - 0.04, top - 0.08), (-d * 0.5 - 0.045, top - 0.32), (-d * 0.5 - 0.004, top - h + 0.06),
                          (-d * 0.5 + 0.004, top - h + 0.07), (-d * 0.5 - 0.024, top - 0.32), (-d * 0.5 - 0.02, top - 0.08), (-d * 0.5 + 0.004, top - 0.035)],
                         sx * 0.07 - 0.032, sx * 0.07 + 0.032, bevel=0.008, segs=3, mat="fabric_nylon", angle=15)
        adj = L.box((sx * 0.07, top - h + 0.11, -d * 0.5 - 0.02), (0.022, 0.012, 0.004), 0.002, 2, "plastic_black")
        straps += [s, adj]
    sternum = L.box((0, top - 0.17, -d * 0.5 - 0.048), (0.075, 0.009, 0.003), 0.0012, 2, "webbing_black")
    sternum_buckle = L.box((0, top - 0.17, -d * 0.5 - 0.052), (0.018, 0.012, 0.005), 0.002, 2, "plastic_black")
    belt = L.side_prism([(-d * 0.5 + 0.01, top - h + 0.03), (-d * 0.5 - 0.06, top - h + 0.01), (-d * 0.5 - 0.06, top - h - 0.04), (-d * 0.5 + 0.01, top - h - 0.035)],
                        -w * 0.5 - 0.04, w * 0.5 + 0.04, bevel=0.01, segs=3, mat="fabric_nylon", angle=15)
    belt_buckle = L.box((0, top - h - 0.012, -d * 0.5 - 0.064), (0.025, 0.016, 0.006), 0.002, 2, "plastic_black")
    handle = L.torus((0, top + 0.03, -0.02), (0, 0, 1), 0.022, 0.006, 32, 8, "webbing_black")
    handle.scale((1.2, 1.0, 0.5), (0, top + 0.03, -0.02))
    patch = L.box((0, cy - 0.04, d * 0.5 + 0.0495), (0.04, 0.022, 0.0015), 0.002, 2, "webbing_black")
    name = L.text("DG TREK 30", (0, cy - 0.04, d * 0.5 + 0.0512), 0.009, (0, 0, 1), (0, 1, 0), 0.0002, "label_white")
    it.part("body").add(body, collar, front, front_seam, *pockets, *comp, patch, name)
    it.part("lid", pivot=(0, top - 0.02, -d * 0.5), mech="lid").add(lid, lid_zip, *lid_straps)
    it.part("straps", mech="straps").add(*straps, sternum, sternum_buckle)
    it.part("hip_belt", mech="straps").add(belt, belt_buckle)
    it.part("handle").add(handle)
    it.socket("grip_r", (0, 0, 0))
    it.socket("back", (0, cy, -d * 0.5))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    return it
