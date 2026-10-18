"""Sling bag (data/items.json "sling_bag"): a small grey-black shoulder bag: a rounded wedge body with a
zipped main compartment (zipper pulls), a flat front organiser pocket with a loop-field patch, a padded
back panel, the shoulder sling with its side-release buckle and slider. Real size 18 x 20 x 8 cm,
palm-held. Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("sling_bag", "item")
    w, h, d = 0.18, 0.2, 0.08
    # Wedge profile (side view, z / y): thicker at the bottom.
    prof = [(-d * 0.5, 0.0), (d * 0.5, 0.0), (d * 0.5, h * 0.45), (d * 0.18, h), (-d * 0.5, h)]
    body = L.side_prism(prof, -w * 0.5, w * 0.5, bevel=0.016, segs=4, mat="fabric_nylon", angle=20)
    back = L.box((0, h * 0.5, -d * 0.5 - 0.006), (w * 0.46, h * 0.46, 0.007), 0.006, 3, "webbing_black")
    pocket = L.box((0, h * 0.33, d * 0.5 + 0.006), (w * 0.4, h * 0.24, 0.007), 0.008, 3, "fabric_canvas")
    loop_patch = L.box((0, h * 0.42, d * 0.5 + 0.0135), (0.04, 0.022, 0.0008), 0.0008, 1, "webbing_black")
    zip_main = L.box((0, h * 0.86, d * 0.28), (w * 0.47, 0.003, 0.004), 0.0012, 2, "webbing_black")
    zip_main.rotate((1, 0, 0), -38, (0, h * 0.86, d * 0.28))
    zip_pocket = L.box((0, h * 0.56, d * 0.5 + 0.013), (w * 0.37, 0.003, 0.0015), 0.0008, 1, "webbing_black")
    pulls = [L.box((x, y, z), (0.0035, 0.011, 0.0012), 0.0008, 2, "nickel") for (x, y, z) in ((0.06, h * 0.83, d * 0.36), (-0.05, h * 0.53, d * 0.5 + 0.015))]
    tabs = [L.box((x, y - 0.016, z + 0.001), (0.004, 0.007, 0.0012), 0.0008, 2, "webbing_black") for (x, y, z) in ((0.06, h * 0.83, d * 0.36), (-0.05, h * 0.53, d * 0.5 + 0.015))]
    # The sling: an arc between the top corners (shortened: the shoulder loop shown cinched).
    sling = L.torus((0, h * 0.9, -d * 0.5 - 0.012), (0, 0, 1), 0.088, 0.005, 48, 8, "webbing_black")
    sling.scale((1.0, 0.9, 0.35), (0, h * 0.9, -d * 0.5 - 0.012))
    sling.cut(L.box((0, h * 0.9 - 0.1, -d * 0.5 - 0.012), (0.15, 0.1, 0.05), 0, 0))
    anchors = [L.box((sx * (w * 0.5 - 0.004), h * 0.92, -d * 0.5 - 0.01), (0.012, 0.016, 0.004), 0.0015, 2, "webbing_black") for sx in (1, -1)]
    buckle = L.box((0.06, h * 1.19, -d * 0.5 - 0.012), (0.012, 0.018, 0.006), 0.002, 2, "plastic_black")
    slider = L.box((-0.065, h * 1.165, -d * 0.5 - 0.012), (0.004, 0.014, 0.006), 0.0015, 2, "plastic_black")
    label = L.text("DG", (0, h * 0.2, d * 0.5 + 0.0135), 0.012, (0, 0, 1), (0, 1, 0), 0.0002, "label_white")
    it.part("body").add(body, back, pocket, loop_patch, zip_main, zip_pocket, *pulls, *tabs, label)
    it.part("sling", mech="straps").add(sling, *anchors, buckle, slider)
    it.socket("grip_r", (0, 0, 0))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    it.hold_small()
    return it
