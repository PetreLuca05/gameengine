"""7.62 rounds (data/items.json "ammo_762"): a small steel ammo can (green paint): lid with the cam latch
(separate), carry handle, stencilled markings. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("ammo_762", "item")
    w, h, d = 0.15, 0.1, 0.065
    can = L.box((0, h * 0.5, 0), (w * 0.5, h * 0.5, d * 0.5), 0.004, 3, "paint_olive")
    ribs = [L.box((0, h * 0.5, sz * (d * 0.5 + 0.0005)), (w * 0.5 - 0.01, 0.003, 0.0006), 0.0006, 1, "paint_olive") for sz in (1, -1) for _ in (0,)]
    lid = L.box((0, h + 0.006, 0), (w * 0.5 + 0.002, 0.006, d * 0.5 + 0.002), 0.003, 3, "paint_olive")
    handle = L.cyl((-0.035, h + 0.016, 0), (0.035, h + 0.016, 0), 0.0035, 16, "steel_parkerized", bevel=0.0005)
    posts = [L.cyl((x, h + 0.012, 0), (x, h + 0.019, 0), 0.0022, 12, "steel_parkerized", bevel=0.0003) for x in (-0.035, 0.035)]
    latch = L.side_prism([(-d * 0.5 - 0.004, h + 0.012), (-d * 0.5 - 0.001, h + 0.012), (-d * 0.5 - 0.001, h - 0.03), (-d * 0.5 - 0.006, h - 0.03)], -0.014, 0.014, bevel=0.0012,
                         segs=2, mat="paint_olive")
    txt = [L.text("CTG 7.62MM", (0, h * 0.62, d * 0.5 + 0.0008), 0.011, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow"),
           L.text("120 LINKED  M80 BALL", (0, h * 0.42, d * 0.5 + 0.0008), 0.0062, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow")]
    it.part("can").add(can, *ribs, *txt)
    it.part("lid", pivot=(0, h + 0.006, d * 0.5), mech="lid").add(lid, handle, *posts)
    it.part("latch", pivot=(0, h + 0.012, -d * 0.5 - 0.003), mech="latch").add(latch)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
