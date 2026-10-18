"""Syringe (data/items.json "syringe", the DNA syringe of docs/REVIVE.md): 10 ml clear barrel with printed
graduations, finger flange, the plunger rod (separate, slides) with its rubber stopper, a green needle hub
with the needle (separate) and a dark red draw inside. Held upright in the fist, needle up (+Y), placed like
the old held box. Procedural (path b)."""
import item_lib as L

R = 0.0076


def build(ctx):
    it = L.Item("syringe", "item")
    barrel = L.ring_lathe([(R - 0.0008, 0), (R, 0), (R, 0.074), (R * 0.55, 0.079), (0.0012, 0.08), (0.0012, 0.0815), (0.0009, 0.0815), (0.0009, 0.08), (R * 0.55 - 0.0008, 0.0782),
                           (R - 0.0008, 0.0735)], (0, 0, 0), (0, 1, 0), 40, "plastic_clear")
    flange = L.top_prism(L.rounded_rect(0.034, 0.016, 0.006, 4), -0.0015, 0.0005, bevel=0.0005, segs=2, mat="plastic_clear")
    flange.cut(L.cyl((0, -0.01, 0), (0, 0.01, 0), R - 0.0008, 32, bevel=0))
    grads = []
    for k in range(11):
        y = 0.008 + k * 0.006
        w = 0.004 if k % 5 == 0 else 0.0022
        grads.append(L.box((0, y, -R - 0.0001), (w * 0.5, 0.00025, 0.00015), 0, 0, "label_black"))
    grads.append(L.text("10 ml", (0.0, 0.074, -R - 0.0002), 0.0022, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"))
    draw = L.cyl((0, 0.031, 0), (0, 0.072, 0), R - 0.0011, 32, "liquid_red", bevel=0)
    hub = L.lathe([(0.0012, 0), (0.0032, 0), (0.0032, 0.004), (0.0022, 0.009), (0.0008, 0.0095), (0.0008, 0)], (0, 0.0812, 0), (0, 1, 0), 24, "plastic_green")
    needle = L.cyl((0, 0.088, 0), (0, 0.112, 0), 0.00042, 12, "steel_stainless", bevel=0)
    tip = L.lathe([(0.00042, 0), (0, 0.0025)], (0, 0.112, 0), (0, 1, 0), 12, "steel_stainless")
    rod_x = L.box((0, 0.0, 0), (0.0048, 0.025, 0.0007), 0.0002, 1, "plastic_white")
    rod_z = L.box((0, 0.0, 0), (0.0007, 0.025, 0.0048), 0.0002, 1, "plastic_white")
    rod_x.move((0, 0.004, 0))
    rod_z.move((0, 0.004, 0))
    thumb = L.cyl((0, -0.021, 0), (0, -0.023, 0), 0.0085, 32, "plastic_white", bevel=0.0004)
    stopper = L.lathe([(0, 0), (R - 0.0011, 0), (R - 0.0009, 0.002), (R - 0.0011, 0.004), (R - 0.0009, 0.006), (R * 0.5, 0.0085), (0, 0.009)], (0, 0.0215, 0), (0, 1, 0), 32,
                      "rubber")
    it.part("barrel").add(barrel, flange, draw, *grads)
    it.part("plunger", pivot=(0, 0.0215, 0), mech="plunger").add(rod_x, rod_z, thumb, stopper)
    it.part("needle", mech="needle").add(hub, needle, tip)
    it.socket("grip_r", (0, 0, 0))
    it.socket("needle", (0, 0.1145, 0))
    it.socket("plunger", (0, -0.023, 0))
    it.hold_small()
    return it
