"""Painkillers (data/items.json "painkillers"): an amber pill bottle with a white child-resistant cap
(separate, ribbed, with the "push down & turn" arrows), a wrap-around pharmacy label and pills inside.
Palm-held. Procedural (path b)."""
import item_lib as L

R = 0.019


def build(ctx):
    it = L.Item("painkillers", "item")
    bottle = L.lathe([(0, 0), (R - 0.002, 0), (R, 0.002), (R, 0.064), (R - 0.0015, 0.066), (R - 0.0015, 0.072), (R - 0.0035, 0.072), (R - 0.0035, 0.003), (0, 0.003)],
                     (0, 0, 0), (0, 1, 0), 56, "glass_amber")
    rings = L.merge(*[L.torus((0, 0.0675 + k * 0.0018, 0), (0, 1, 0), R - 0.0013, 0.0004, 48, 6, "glass_amber") for k in range(3)])
    label = L.ring_lathe([(R + 0.00005, 0), (R + 0.0002, 0), (R + 0.0002, 0.042), (R + 0.00005, 0.042)], (0, 0.012, 0), (0, 1, 0), 56, "label_white")
    band = L.ring_lathe([(R + 0.0002, 0), (R + 0.0003, 0), (R + 0.0003, 0.008), (R + 0.0002, 0.008)], (0, 0.044, 0), (0, 1, 0), 56, "label_red")
    txt = [L.text("PAINKILLERS", (0, 0.034, -R - 0.0004), 0.0042, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("IBUPROFEN 400 mg", (0, 0.027, -R - 0.0004), 0.0028, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("TAKE 1 TABLET  EVERY 6 HOURS", (0, 0.021, -R - 0.0004), 0.0019, (0, 0, -1), (0, 1, 0), 0.0001, "label_black"),
           L.text("RX 448120", (0, 0.047, -R - 0.0005), 0.0026, (0, 0, -1), (0, 1, 0), 0.0001, "label_white")]
    pills = []
    for k in range(9):
        import math
        a = k * 2.4
        rr = 0.007 + 0.006 * ((k * 37) % 5) / 5.0
        p = L.sphere((rr * math.cos(a), 0.0065 + 0.005 * (k % 3), rr * math.sin(a)), 0.0042, 12, 6, "plastic_white")
        p.scale((1.0, 0.45, 1.0), (rr * math.cos(a), 0.0065 + 0.005 * (k % 3), rr * math.sin(a)))
        pills.append(p)
    cap = L.lathe([(0, 0), (R + 0.0012, 0), (R + 0.0014, 0.002), (R + 0.0014, 0.0125), (R + 0.0006, 0.014), (0, 0.014)], (0, 0.0645, 0), (0, 1, 0), 64, "plastic_white")
    cap.cut(L.knurl((0, 0.0655, 0), (0, 0.0765, 0), R + 0.0014, 48, 0.0005))
    arrows = L.text("PUSH DOWN & TURN", (0, 0.0786, 0), 0.0024, (0, 1, 0), (0, 0, -1), 0.0001, "label_black")
    it.part("bottle").add(bottle, rings, label, band, *txt, *pills)
    it.part("cap", mech="cap").add(cap, arrows)
    it.socket("grip_r", (0, 0, 0))
    it.socket("cap", (0, 0.0785, 0))
    it.hold_small()
    return it
