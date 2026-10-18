"""Armor plate (data/items.json "armor"): a curved SAPI-cut ballistic plate (shooter's cut corners) in its
black ripstop cover with the printed spec label and the edge binding. Carried by its top edge (grip_r).
Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("armor", "item")
    w, h, t = 0.25, 0.32, 0.022
    pts = [(-w * 0.5, -h), (w * 0.5, -h), (w * 0.5, -0.07), (w * 0.5 - 0.06, 0.0), (-w * 0.5 + 0.06, 0.0), (-w * 0.5, -0.07)]
    plate = L._extrude_poly(pts, lambda p: L.Vector((p[0], p[1], -t * 0.5)), L.Vector((0, 0, t)), "fabric_nylon", 0.005, 3)
    # Single curve: bend the plate about Y (x -> z offset).
    for v in plate.bm.verts:
        v.co.z += (v.co.x / (w * 0.5)) ** 2 * 0.022
    binding = L.merge(*[L.box(((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, -t * 0.5 - 0.0004), (abs(a[0] - b[0]) * 0.5 + 0.004, abs(a[1] - b[1]) * 0.5 + 0.004, 0.0006), 0.001, 1,
                              "webbing_black") for (a, b) in zip(pts, pts[1:] + pts[:1])])
    label = L.box((0, -h * 0.55, -t * 0.5 - 0.0012), (0.06, 0.04, 0.0005), 0.001, 1, "label_white")
    txt = [L.text("LEVEL IV  STAND ALONE", (0, -h * 0.52, -t * 0.5 - 0.0018), 0.0075, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("DG ARMOR  SAPI M", (0, -h * 0.58, -t * 0.5 - 0.0018), 0.006, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("STRIKE FACE  THIS SIDE OUT", (0, -h * 0.66, -t * 0.5 - 0.0018), 0.0045, (0, 0, -1), (0, 1, 0), 0.0001, "label_red")]
    it.part("plate").add(plate, binding, label, *txt)
    it.socket("grip_r", (0, 0, 0))
    return it
