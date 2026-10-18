"""Balaclava (data/items.json "balaclava"): a black knit face cover folded flat: the folded body (soft
rounded slab with a ribbed hem), the eye opening showing on the top fold, the stitched seam and a woven
size label. Real size 16 x 4 x 20 cm, palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("balaclava", "item")
    w, t, d = 0.16, 0.02, 0.2
    body = L.top_prism(L.rounded_rect(w, d, 0.035, 6), 0.0, t, bevel=0.007, segs=4, mat="webbing_black", angle=15)
    top_fold = L.top_prism(L.rounded_rect(w * 0.92, d * 0.55, 0.03, 6, 0.0, -d * 0.2), t, t + 0.012, bevel=0.005, segs=3, mat="webbing_black", angle=15)
    eye = L.top_prism(L.rounded_rect(0.1, 0.03, 0.013, 5, 0.0, -d * 0.28), t + 0.004, t + 0.02, bevel=0, segs=0)
    top_fold.cut(eye)
    hem = L.array(L.box((-w * 0.5 + 0.008, t * 0.5, d * 0.5 - 0.004), (0.0016, t * 0.5 + 0.0006, 0.004), 0.0008, 1, "webbing_black"), 24, (0.0063, 0, 0))
    seam = L.box((0, t + 0.0122, -d * 0.2), (0.0008, 0.0006, d * 0.25), 0.0003, 1, "fabric_nylon")
    label = L.box((w * 0.32, t * 0.5, d * 0.5 + 0.001), (0.012, 0.007, 0.0008), 0.0005, 1, "label_white")
    size = L.text("L", (w * 0.32, t * 0.5, d * 0.5 + 0.0019), 0.006, (0, 0, 1), (0, 1, 0), 0.0001, "label_black")
    it.part("body").add(body, top_fold, hem, seam, label, size)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
