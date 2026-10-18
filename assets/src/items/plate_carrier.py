"""Plate carrier (data/items.json "plate_carrier"): front and back plate bags in olive nylon with laser-cut
MOLLE slots and a loop-field ID panel, padded shoulder straps with cable channels, the elastic
cummerbund with its flaps, the kangaroo pouch under the front plate, and the drag handle on top of the
back bag (grip_r at the origin). Procedural (path b)."""
import _gear as G
import item_lib as L


def plate_bag(z, sign, w, h, t, top):
    """One plate bag: SAPI-cut outline (shooter's cut top corners) extruded through t, its face at z."""
    pts = [(-w * 0.5, top - h), (w * 0.5, top - h), (w * 0.5, top - 0.06), (w * 0.5 - 0.055, top), (-w * 0.5 + 0.055, top), (-w * 0.5, top - 0.06)]
    bag = L._extrude_poly(pts, lambda p: L.Vector((p[0], p[1], z - sign * t * 0.5)), L.Vector((0, 0, sign * t)), "fabric_nylon", 0.006, 3)
    # Laser-cut MOLLE: slot rows cut into the outer face.
    slots = []
    for row in range(4):
        y = top - h + 0.03 + row * 0.03
        for k in range(5):
            slots.append(L.box((-0.08 + k * 0.04, y, z + sign * t * 0.5), (0.014, 0.0035, 0.004), 0, 0))
    bag.cut(L.merge(*slots))
    return bag


def build(ctx):
    it = L.Item("plate_carrier", "item")
    w, h, t = 0.25, 0.31, 0.03
    top = -0.04
    zf, zb = 0.22, 0.0   # front / back bag centre planes (the drag handle on the back bag is the hand)
    front = plate_bag(zf, 1, w, h, t, top)
    back = plate_bag(zb, -1, w, h, t, top)
    id_panel = L.box((0, top - 0.04, zf + t * 0.5 + 0.0012), (0.05, 0.022, 0.0012), 0.0015, 2, "webbing_black")
    patch = L.text("DG", (0, top - 0.04, zf + t * 0.5 + 0.0026), 0.014, (0, 0, 1), (0, 1, 0), 0.0002, "label_white")
    kangaroo = L.box((0, top - h + 0.05, zf + t * 0.5 + 0.012), (w * 0.42, 0.045, 0.012), 0.008, 3, "fabric_nylon")
    straps = []
    for sx in (1, -1):
        x = sx * 0.075
        # An inverted U over the shoulder from the back bag's top to the front bag's.
        s = L.side_prism([(zb - 0.004, top - 0.03), (zb + 0.03, top + 0.03), (zf - 0.03, top + 0.03), (zf + 0.004, top - 0.03), (zf - 0.014, top - 0.03),
                          (zf - 0.04, top + 0.012), (zb + 0.04, top + 0.012), (zb + 0.014, top - 0.03)], x - 0.024, x + 0.024, bevel=0.005, segs=3,
                         mat="fabric_nylon", angle=15)
        cable = L.box((x, top + 0.033, (zb + zf) * 0.5), (0.008, 0.004, 0.04), 0.002, 2, "webbing_black")
        straps += [s, cable]
    cumm = []
    for sx in (1, -1):
        c = L.side_prism([(zb + 0.01, top - h + 0.02), (zf - 0.01, top - h + 0.02), (zf - 0.01, top - h + 0.14), (zb + 0.01, top - h + 0.14)],
                         sx * (w * 0.5 + 0.002), sx * (w * 0.5 + 0.016), bevel=0.004, segs=2, mat="fabric_nylon", angle=15)
        flap = L.box((sx * (w * 0.5 - 0.02), top - h + 0.08, zf + t * 0.5 + 0.004), (0.035, 0.06, 0.004), 0.004, 2, "fabric_nylon")
        cumm += [c, flap]
    handle = L.torus((0, top + 0.022, zb), (0, 0, 1), 0.022, 0.006, 32, 8, "webbing_black")
    handle.scale((1.6, 1.0, 0.6), (0, top + 0.022, zb))
    handle.cut(L.box((0, top - 0.04, zb), (0.06, 0.04, 0.05), 0, 0))   # the part sewn into the bag
    it.part("front").add(front, id_panel, patch, kangaroo)
    it.part("back").add(back)
    it.part("straps", mech="straps").add(*straps)
    it.part("cummerbund", mech="straps").add(*cumm)
    it.part("handle").add(handle)
    it.socket("grip_r", (0, 0, 0))
    it.socket("front_plate", (0, top - h * 0.5, zf))
    it.socket("back_plate", (0, top - h * 0.5, zb))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    return it
