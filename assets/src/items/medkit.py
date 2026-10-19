"""Medkit (data/items.json "medkit"): an IFAK pouch: red Cordura body (rounded box) with a zip-around lid
(separate part, slightly open), a white cross patch, MOLLE webbing on the back, a grab loop and a zipper
pull; contents peeking at the lid gap. Real size 18 x 12 x 8 cm, palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("medkit", "item")
    w, h, d = 0.18, 0.12, 0.08
    body = L.box((0, h * 0.35, 0), (w * 0.5, h * 0.35, d * 0.5), 0.016, 6, "fabric_red")
    lid = L.box((0, h * 0.85, 0), (w * 0.5 - 0.001, h * 0.16, d * 0.5 - 0.001), 0.016, 6, "fabric_red")
    lid.cut(L.box((0, h * 0.7, 0), (w, h * 0.012, d), 0, 0))
    seam = L.box((0, h * 0.7 + 0.0005, 0), (w * 0.5 + 0.0012, 0.0028, d * 0.5 + 0.0012), 0.0025, 2, "webbing_black")
    teeth = L.array(L.box((-w * 0.5 + 0.012, h * 0.7 + 0.0005, d * 0.5 + 0.0015), (0.0006, 0.0018, 0.0006), 0, 0, "nickel"), 60, (0.0026, 0, 0))
    pull = L.side_prism([(d * 0.5 + 0.002, h * 0.7 + 0.002), (d * 0.5 + 0.004, h * 0.7 + 0.002), (d * 0.5 + 0.004, h * 0.7 - 0.016), (d * 0.5 + 0.002, h * 0.7 - 0.016)],
                        w * 0.42 - 0.004, w * 0.42 + 0.004, bevel=0.0006, segs=1, mat="nickel")
    tab = L.box((w * 0.42, h * 0.7 - 0.022, d * 0.5 + 0.003), (0.004, 0.007, 0.0012), 0.001, 2, "webbing_black")
    cross_v = L.box((0, h * 0.45, d * 0.5 + 0.0008), (0.008, 0.024, 0.0008), 0.0006, 1, "fabric_gauze")
    cross_h = L.box((0, h * 0.45, d * 0.5 + 0.0008), (0.024, 0.008, 0.0008), 0.0006, 1, "fabric_gauze")
    patch = L.box((0, h * 0.45, d * 0.5 + 0.0004), (0.034, 0.034, 0.0006), 0.003, 2, "webbing_black")
    molle = [L.box((0, h * 0.18 + k * 0.026, -d * 0.5 - 0.0012), (w * 0.47, 0.0065, 0.0012), 0.0008, 1, "webbing_black") for k in range(3)]
    tacks = [L.box((x, h * 0.18 + k * 0.026, -d * 0.5 - 0.0024), (0.0012, 0.0066, 0.0006), 0, 0, "fabric_red") for k in range(3) for x in (-0.06, -0.02, 0.02, 0.06)]
    loop = L.torus((0, h + 0.008, 0), (0, 0, 1), 0.012, 0.0028, 32, 8, "webbing_black")
    loop.scale((1.4, 1.0, 0.6), (0, h + 0.008, 0))
    label = L.text("IFAK", (0, h * 0.18, d * 0.5 + 0.0004), 0.012, (0, 0, 1), (0, 1, 0), 0.0003, "fabric_gauze")
    gauze = L.cyl((-0.04, h * 0.7, 0.0), (-0.04, h * 0.7 + 0.004, 0.0), 0.022, 24, "fabric_gauze", bevel=0.001)
    # Stitching: rows of short stitches along the vertical edges, the base seam and round the patch.
    st = []
    for sx in (1, -1):
        for sz in (1, -1):
            for k in range(26):
                st.append(L.box((sx * (w * 0.5 - 0.004), 0.006 + k * 0.0032, sz * (d * 0.5 + 0.0003)), (0.0003, 0.0011, 0.0003), 0, 0, "webbing_black"))
    for k in range(48):
        x = -w * 0.5 + 0.012 + k * 0.0034
        st.append(L.box((x, 0.004, d * 0.5 + 0.0003), (0.0011, 0.0003, 0.0003), 0, 0, "webbing_black"))
        st.append(L.box((x, 0.004, -d * 0.5 - 0.0003), (0.0011, 0.0003, 0.0003), 0, 0, "webbing_black"))
    for k in range(20):
        t = k / 20.0
        for (px, py) in ((-0.034 + t * 0.068, 0.034), (-0.034 + t * 0.068, -0.034), (0.034, -0.034 + t * 0.068), (-0.034, -0.034 + t * 0.068)):
            st.append(L.box((px, h * 0.45 + py, d * 0.5 + 0.0012), (0.0007, 0.0007, 0.0003), 0, 0, "fabric_red"))
    velcro = L.box((0, h * 0.18, -d * 0.5 - 0.0026), (0.045, 0.012, 0.0008), 0.001, 1, "fabric_nylon")
    loops = [L.torus((sx * (w * 0.5 + 0.002), h * 0.42, 0), (1, 0, 0), 0.01, 0.0022, 24, 6, "webbing_black") for sx in (1, -1)]
    it.part("body").add(body, seam, teeth, patch, cross_v, cross_h, label, *molle, *tacks, gauze, velcro, *loops)
    it.part("stitching", detail=True).add(L.merge(*st))
    it.part("lid", pivot=(0, h * 0.7, -d * 0.5), mech="lid").add(lid, loop)
    it.part("zipper", mech="zip").add(pull, tab)
    it.socket("grip_r", (0, 0, 0))
    it.socket("lid", (0, h * 0.7, 0))
    it.hold_small()
    return it
