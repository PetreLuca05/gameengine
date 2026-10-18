"""Heavy stock (assets/attachments/heavy_stock.json): adjustable precision stock on its own buffer tube:
skeleton aluminium chassis, cheek riser on posts with a knob, length-of-pull spacers, thick rubber butt
pad, monopod knob, QD sling cups. Procedural (path b). Attachment space: the stock slot."""
import item_lib as L


def build(ctx):
    it = L.Item("att_heavy_stock", "attachment")
    tube = L.cyl((0, 0, 0.0), (0, 0, 0.12), 0.0148, 40, "aluminium_anodized", bevel=0.0008)
    nut = L.ring_lathe([(0.0148, 0), (0.019, 0), (0.019, 0.008), (0.0148, 0.008)], (0, 0, 0.002), (0, 0, 1), 40, "steel_parkerized", bevel=0.0005)
    chassis = L.side_prism([(0.02, 0.024), (0.2, 0.026), (0.232, 0.03), (0.232, -0.072), (0.21, -0.074), (0.16, -0.05), (0.09, -0.03), (0.02, -0.022)], -0.0215, 0.0215,
                           bevel=0.003, segs=3, mat="aluminium_anodized", angle=20)
    chassis.cut(L.cyl((0, 0, -0.01), (0, 0, 0.13), 0.0152, 32, bevel=0))
    for (a, b) in (((0.11, 0.018), (0.19, 0.02)), ((0.13, -0.022), (0.2, -0.04))):
        chassis.cut(L.side_prism([(a[0], a[1]), (b[0], b[1]), (b[0], b[1] - 0.026), (a[0] + 0.02, a[1] - 0.02)], -0.03, 0.03, 0, 0))
    cheek = L.side_prism([(0.08, 0.062), (0.215, 0.062), (0.222, 0.054), (0.088, 0.05)], -0.017, 0.017, bevel=0.003, segs=3, mat="polymer_matte")
    posts = [L.cyl((0, 0.025, z), (0, 0.052, z), 0.004, 16, "steel_dark", bevel=0.0004) for z in (0.11, 0.19)]
    knob = L.cyl((0.0215, 0.035, 0.15), (0.03, 0.035, 0.15), 0.0072, 24, "aluminium_anodized", bevel=0.0008)
    knob.cut(L.knurl((0.022, 0.035, 0.15), (0.03, 0.035, 0.15), 0.0072, 20, 0.0005))
    spacers = [L.side_prism([(0.232 + k * 0.004, 0.03), (0.2355 + k * 0.004, 0.03), (0.2355 + k * 0.004, -0.072), (0.232 + k * 0.004, -0.072)], -0.022, 0.022, 0.0006, 1,
                            "polymer_matte") for k in range(2)]
    pad = L.side_prism([(0.24, 0.053), (0.256, 0.051), (0.257, -0.076), (0.24, -0.078)], -0.026, 0.026, bevel=0.004, segs=3, mat="rubber")
    pad.cut(L.merge(*[L.box((0, 0.045 - k * 0.009, 0.2565), (0.028, 0.0013, 0.0013), 0, 0) for k in range(14)]))
    mono = L.cyl((0, -0.074, 0.19), (0, -0.09, 0.19), 0.0085, 28, "aluminium_anodized", bevel=0.001)
    mono.cut(L.knurl((0, -0.075, 0.19), (0, -0.089, 0.19), 0.0085, 24, 0.0005))
    qds = [L.tube((sx * 0.021, -0.04, 0.2), (sx * 0.0255, -0.04, 0.2), 0.0058, 0.0039, 20, "steel_parkerized") for sx in (1, -1)]
    it.part("tube").add(tube, nut)
    it.part("chassis").add(chassis, *spacers, *qds)
    it.part("cheek_riser", mech="cheek_riser").add(cheek, knob, *posts)
    it.part("butt_pad").add(pad)
    it.part("monopod", mech="monopod", detail=True).add(mono)
    it.socket("mount", (0, 0, 0))
    it.socket("butt", (0, -0.012, 0.257))
    return it
