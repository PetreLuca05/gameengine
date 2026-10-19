"""Canned food (data/items.json "canned_food"): a tin can with rolled seams, body ribs, a pull-ring lid
(separate: opened when eaten) and a wrap-around printed paper label ("BAKED BEANS"). Palm-held.
Procedural (path b)."""
import item_lib as L

R = 0.0365
H = 0.1


def build(ctx):
    it = L.Item("canned_food", "item")
    body = L.lathe([(0, 0.002), (R - 0.004, 0.001), (R - 0.0025, 0.0), (R + 0.0008, 0.0012), (R + 0.0008, 0.004), (R, 0.006), (R, H - 0.006), (R + 0.0008, H - 0.004),
                    (R + 0.0008, H - 0.0012), (R - 0.0025, H), (R - 0.0028, H - 0.0035), (0, H - 0.0035)], (0, 0, 0), (0, 1, 0), 64, "tin")
    ribs = L.merge(*[L.torus((0, 0.012 + k * 0.0095, 0), (0, 1, 0), R, 0.0007, 64, 6, "tin") for k in range(9)])
    label = L.ring_lathe([(R + 0.0001, 0), (R + 0.0004, 0), (R + 0.0004, H - 0.016), (R + 0.0001, H - 0.016)], (0, 0.008, 0), (0, 1, 0), 64, "label_red")
    band = L.ring_lathe([(R + 0.0004, 0), (R + 0.0005, 0), (R + 0.0005, 0.03), (R + 0.0004, 0.03)], (0, 0.035, 0), (0, 1, 0), 64, "label_white")
    txt = [L.text("BAKED", (0, 0.056, -R - 0.0007), 0.0095, (0, 0, -1), (0, 1, 0), 0.00015, "label_red"),
           L.text("BEANS", (0, 0.043, -R - 0.0007), 0.0095, (0, 0, -1), (0, 1, 0), 0.00015, "label_red"),
           L.text("IN TOMATO SAUCE  415 g", (0, 0.024, -R - 0.0006), 0.003, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("DEADGROUND FOODS", (0, 0.08, -R - 0.0006), 0.0034, (0, 0, -1), (0, 1, 0), 0.0001, "label_white")]
    lid = L.cyl((0, H - 0.0035, 0), (0, H - 0.0028, 0), R - 0.0028, 56, "tin", bevel=0.0002)
    score = L.torus((0, H - 0.0027, 0), (0, 1, 0), R - 0.006, 0.0004, 56, 6, "tin")
    rivet = L.cyl((0, H - 0.0028, 0.012), (0, H - 0.0015, 0.012), 0.002, 16, "tin", bevel=0.0003)
    ring = L.top_prism(L.rounded_rect(0.016, 0.026, 0.008, 6), H - 0.0022, H - 0.0012, bevel=0.0003, segs=1, mat="aluminium_bare")
    ring.move((0, 0, 0.004))
    ring.cut(L.top_prism(L.rounded_rect(0.009, 0.016, 0.0045, 6), H - 0.004, H + 0.001, 0, 0).move((0, 0, 0.002)))
    it.part("can").add(body, ribs, label, band, *txt)
    it.part("lid", mech="lid").add(lid, score, rivet, ring)
    it.socket("grip_r", (0, 0, 0))
    it.socket("pull_ring", (0, H - 0.001, 0.0))
    it.hold_small()
    return it
