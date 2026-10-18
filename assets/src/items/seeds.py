"""Seed packet (data/items.json "seeds"): a paper seed envelope: the printed front (a carrot), the
crimped top seal, the back flap seam, slight pillow from the seeds inside. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("seeds", "item")
    w, h, t = 0.08, 0.12, 0.006
    env = L.box((0, h * 0.5, 0), (w * 0.5, h * 0.5, t * 0.5), 0.0022, 3, "paper_label")
    for v in env.bm.verts:
        v.co.z *= 0.6 + 0.4 * (1 - (v.co.y / h - 0.45) ** 2 * 3.0)
    crimp = L.box((0, h - 0.004, 0), (w * 0.5, 0.004, t * 0.32), 0.0005, 1, "paper_label")
    crimp.cut(L.merge(*[L.box((-w * 0.5 + k * 0.002, h - 0.004, 0), (0.0004, 0.0042, t), 0, 0) for k in range(41)]))
    art = L.box((0, h * 0.55, t * 0.5 + 0.0003), (w * 0.4, h * 0.3, 0.0003), 0.001, 1, "label_green")
    carrot = L.lathe([(0, 0), (0.008, 0.008), (0.006, 0.045), (0, 0.052)], (0, h * 0.42, t * 0.5 + 0.0005), (0, 1, 0), 12, "paint_orange")
    carrot.scale((1.0, 1.0, 0.08), (0, h * 0.42, t * 0.5 + 0.0005))
    txt = [L.text("CARROT", (0, h * 0.2, t * 0.5 + 0.0007), 0.01, (0, 0, 1), (0, 1, 0), 0.0001, "label_black"),
           L.text("NANTES  ORGANIC SEEDS", (0, h * 0.13, t * 0.5 + 0.0007), 0.0045, (0, 0, 1), (0, 1, 0), 0.0001, "label_black")]
    it.part("packet").add(env, crimp, art, carrot, *txt)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
