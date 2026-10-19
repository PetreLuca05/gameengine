"""School backpack (data/items.json "backpack", docs/CHAPTER_01.md): a blue nylon daypack: padded body,
zipped main compartment and front pocket (zippers with pulls), side mesh bottle pocket, padded shoulder
straps with adjusters, top grab handle (grip_r at the origin), a name tag. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("backpack", "item")
    w, h, d = 0.3, 0.4, 0.15
    top = -0.012
    body = L.box((0, top - h * 0.5, 0), (w * 0.5, h * 0.5, d * 0.5), 0.05, 5, "fabric_blue")
    pocket = L.box((0, top - h * 0.68, d * 0.5 + 0.02), (w * 0.38, h * 0.22, 0.03), 0.03, 4, "fabric_blue")
    zips = [L.box((0, top - h * 0.12, d * 0.5 - 0.004), (w * 0.43, 0.004, 0.006), 0.002, 2, "webbing_black"),
            L.box((0, top - h * 0.5, d * 0.5 + 0.045), (w * 0.33, 0.004, 0.006), 0.002, 2, "webbing_black")]
    pulls = [L.box((x, top - y, z), (0.004, 0.012, 0.0015), 0.001, 2, "nickel") for (x, y, z) in ((0.1, h * 0.12 + 0.012, d * 0.5 + 0.002), (-0.08, h * 0.5 + 0.012, d * 0.5 + 0.052))]
    side = L.box((w * 0.5 + 0.015, top - h * 0.75, 0), (0.018, h * 0.15, d * 0.38), 0.012, 3, "webbing_black")
    straps = []
    for sx in (1, -1):
        s = L.side_prism([(-d * 0.5 - 0.002, top - 0.03), (-d * 0.5 - 0.035, top - 0.08), (-d * 0.5 - 0.04, top - 0.3), (-d * 0.5 - 0.004, top - h + 0.03),
                          (-d * 0.5 + 0.004, top - h + 0.04), (-d * 0.5 - 0.022, top - 0.3), (-d * 0.5 - 0.018, top - 0.08), (-d * 0.5 + 0.004, top - 0.035)],
                         sx * 0.065 - 0.03, sx * 0.065 + 0.03, bevel=0.008, segs=3, mat="fabric_blue", angle=15)
        buckle = L.box((sx * 0.065, top - h + 0.07, -d * 0.5 - 0.016), (0.02, 0.012, 0.004), 0.002, 2, "plastic_black")
        straps += [s, buckle]
    handle = L.torus((0, top + 0.004, -0.01), (0, 0, 1), 0.02, 0.006, 32, 8, "webbing_black")
    handle.scale((1.2, 1.0, 0.5), (0, top + 0.004, -0.01))
    tag = L.box((-w * 0.5 + 0.03, top - h * 0.2, d * 0.5 + 0.004), (0.022, 0.014, 0.0015), 0.002, 2, "label_white")
    name = L.text("JULES", (-w * 0.5 + 0.03, top - h * 0.2, d * 0.5 + 0.0058), 0.008, (0, 0, 1), (0, 1, 0), 0.0002, "label_black")
    it.part("body").add(body, pocket, side, *zips, *pulls, tag, name)
    it.part("straps", mech="straps").add(*straps)
    it.part("handle").add(handle)
    it.socket("grip_r", (0, 0, 0))
    it.socket("back", (0, top - h * 0.5, -d * 0.5))
    return it
