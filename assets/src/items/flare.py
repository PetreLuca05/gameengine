"""Road flare (data/items.json "flare"): a red paper-wound fusee with its printed warnings, the striker cap
(separate: struck off to light), the igniter button at the end and the wire stand. Palm-held, the lit end
up (+Y). Procedural (path b)."""
import item_lib as L

R = 0.0155


def build(ctx):
    it = L.Item("flare", "item")
    tube = L.lathe([(0, 0), (R - 0.0008, 0), (R, 0.001), (R, 0.2), (R - 0.0012, 0.201), (0, 0.201)], (0, 0, 0), (0, 1, 0), 48, "paint_red")
    wraps = L.merge(*[L.torus((0, 0.012 + k * 0.03, 0), (0, 1, 0), R, 0.00025, 48, 4, "paint_red") for k in range(7)])
    band = L.ring_lathe([(R + 0.0001, 0), (R + 0.0003, 0), (R + 0.0003, 0.05), (R + 0.0001, 0.05)], (0, 0.07, 0), (0, 1, 0), 48, "label_white")
    txt = [L.text("FUSEE", (0, 0.105, -R - 0.0004), 0.009, (0, 0, -1), (0, 1, 0), 0.00012, "label_red"),
           L.text("30 MIN  DANGER  FLAMMABLE", (0, 0.09, -R - 0.0004), 0.0028, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("REMOVE CAP  STRIKE", (0, 0.082, -R - 0.0004), 0.0026, (0, 0, -1), (0, 1, 0), 0.0001, "label_black")]
    button = L.lathe([(0, 0), (R * 0.75, 0), (R * 0.75, 0.004), (R * 0.5, 0.006), (0, 0.0065)], (0, 0.201, 0), (0, 1, 0), 32, "paint_black")
    cap = L.lathe([(0, 0), (R + 0.0012, 0), (R + 0.0012, 0.028), (R + 0.0005, 0.03), (0, 0.03)], (0, 0.182, 0), (0, 1, 0), 48, "plastic_black")
    cap.cut(L.cyl((0, 0.18, 0), (0, 0.208, 0), R + 0.0002, 48, bevel=0))
    striker = L.cyl((0, 0.2115, 0), (0, 0.2125, 0), R * 0.9, 32, "cardboard", bevel=0)
    stand = L.cyl((0, 0.0005, 0), (0, -0.028, 0.0), 0.0009, 8, "steel_bare", bevel=0)
    it.part("tube").add(tube, wraps, band, button, stand, *txt)
    it.part("cap", mech="cap").add(cap, striker)
    it.socket("grip_r", (0, 0, 0))
    it.socket("ignition", (0, 0.2075, 0))
    it.hold_small()
    return it
