"""Medical case (data/items.json "med_case"): a white polymer first-aid hard case: two shell halves with
rounded corners and the red seam band, the red cross on the lid, two snap latches and the moulded
carry handle (separate lid part), a printed contents label. Real size 22 x 9 x 16 cm, palm-held.
Procedural (path b)."""
import _gear as G
import item_lib as L


def build(ctx):
    it = L.Item("med_case", "item")
    w, h, d = 0.22, 0.09, 0.16
    base = L.top_prism(L.rounded_rect(w, d, 0.02, 5), 0.0, h * 0.55, bevel=0.006, segs=3, mat="plastic_white", angle=20)
    lid = L.top_prism(L.rounded_rect(w, d, 0.02, 5), h * 0.55, h, bevel=0.008, segs=3, mat="plastic_white", angle=20)
    band = L.top_prism(L.rounded_rect(w + 0.003, d + 0.003, 0.021, 5), h * 0.53, h * 0.57, bevel=0.0012, segs=2, mat="plastic_red", angle=20)
    cross_v = L.box((0, h + 0.0008, 0), (0.011, 0.0009, 0.034), 0.0006, 1, "paint_red")
    cross_h = L.box((0, h + 0.0008, 0), (0.034, 0.0009, 0.011), 0.0006, 1, "paint_red")
    latches = []
    for x in (-0.08, 0.08):
        latches.append(L.box((x, h * 0.55, d * 0.5 + 0.004), (0.014, 0.016, 0.004), 0.0018, 2, "plastic_red"))
        latches.append(L.box((x, h * 0.55 + 0.014, d * 0.5 + 0.0065), (0.012, 0.003, 0.002), 0.001, 1, "plastic_red"))
    # The carry handle on the front at the seam: a grip bar on two posts.
    yh = h * 0.55
    handle = L.merge(L.box((0, yh, d * 0.5 + 0.022), (0.045, 0.006, 0.006), 0.0025, 3, "plastic_white"),
                     *[L.box((sx * 0.045, yh, d * 0.5 + 0.012), (0.006, 0.006, 0.012), 0.0025, 3, "plastic_white") for sx in (1, -1)])
    label = L.box((0, h * 0.28, d * 0.5 + 0.0006), (0.05, 0.016, 0.0006), 0.0005, 1, "label_white")
    text = [L.text("FIRST AID", (0, h * 0.32, d * 0.5 + 0.0016), 0.009, (0, 0, 1), (0, 1, 0), 0.0002, "label_red"),
            L.text("DG MEDICAL  TRAUMA", (0, h * 0.22, d * 0.5 + 0.0016), 0.0045, (0, 0, 1), (0, 1, 0), 0.0001, "label_black")]
    it.part("case").add(base, band, label, *text)
    it.part("lid", pivot=(0, h * 0.55, -d * 0.5), mech="lid").add(lid, cross_v, cross_h)
    it.part("latches", mech="latch").add(*latches)
    it.part("handle", mech="handle").add(handle)
    it.socket("grip_r", (0, 0, 0))
    it.socket("lid", (0, h * 0.55, 0))
    G.face_front(it)   # the +Z face (pockets, labels, latches) towards the camera
    it.hold_small()
    return it
