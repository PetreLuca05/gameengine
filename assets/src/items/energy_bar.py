"""Energy bar (data/items.json "energy_bar"): a pillow-pack wrapper: foil-lined printed film with crimped
end seals (serrated), the fin seal along the back, printed name and flavour. Palm-held.
Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("energy_bar", "item")
    L_, w, h = 0.12, 0.036, 0.016
    body = L.box((0, 0, 0), (L_ * 0.5 - 0.012, h * 0.5, w * 0.5), 0.0065, 4, "label_yellow")
    for sx in (1, -1):
        seal = L.top_prism([(sx * (L_ * 0.5 - 0.016), -w * 0.5 + 0.001), (sx * L_ * 0.5, -w * 0.5 - 0.0005), (sx * L_ * 0.5, w * 0.5 + 0.0005), (sx * (L_ * 0.5 - 0.016), w * 0.5 - 0.001)],
                           -0.0008, 0.0008, bevel=0.0003, segs=1, mat="label_yellow")
        seal.cut(L.merge(*[L.box((sx * L_ * 0.5, 0, -w * 0.5 + k * 0.0026), (0.0012, 0.002, 0.0007), 0, 0).rotate((0, 1, 0), 45, (sx * L_ * 0.5, 0, -w * 0.5 + k * 0.0026))
                           for k in range(15)]))
        seal.cut(L.merge(*[L.box((sx * (L_ * 0.5 - 0.007 - k * 0.0018), 0.0009, 0), (0.0004, 0.0003, w), 0, 0) for k in range(4)]))
        it.part("seal_%s" % ("r" if sx > 0 else "l")).add(seal)
    fin = L.box((0, -h * 0.5 - 0.0008, 0.004), (L_ * 0.5 - 0.014, 0.0008, 0.004), 0.0003, 1, "foil")
    band = L.box((0, h * 0.5 - 0.0002, 0), (0.03, 0.0004, w * 0.5 - 0.004), 0.0003, 1, "label_red")
    txt = [L.text("MAX ENERGY", (0, h * 0.5 + 0.0003, 0.002), 0.0068, (0, 1, 0), (0, 0, -1), 0.00012, "label_white"),
           L.text("PEANUT & CARAMEL  60 g", (0, h * 0.5 + 0.0003, -0.009), 0.0028, (0, 1, 0), (0, 0, -1), 0.0001, "label_black")]
    it.part("wrapper").add(body, fin, band, *txt)
    it.socket("grip_r", (0, 0, 0))
    it.socket("tear_notch", (L_ * 0.5 - 0.002, 0, 0))
    it.hold_small()
    return it
