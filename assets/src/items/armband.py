"""Team armband (data/items.json "armband"): a blue elastic band (a flattened loop, the sewn overlap) with
the loop-field patch carrying the team mark. Real size 10 x 5 x 3 cm (laid flat), palm-held.
Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("armband", "item")
    band = L.ring_lathe([(0.045, -0.025), (0.0475, -0.025), (0.0475, 0.025), (0.045, 0.025)], (0, 0, 0), (0, 1, 0), 64, "fabric_blue", bevel=0.0006)
    band.scale((1.0, 1.0, 0.32))
    overlap = L.box((-0.047, 0, 0), (0.0022, 0.024, 0.009), 0.0012, 2, "fabric_blue")
    stitch = L.array(L.box((-0.0494, -0.021, 0), (0.0004, 0.0012, 0.0075), 0.0002, 1, "label_white"), 8, (0, 0.006, 0))
    patch = L.box((0.0482, 0, 0), (0.0012, 0.019, 0.011), 0.001, 2, "webbing_black")
    mark = L.box((0.0496, 0, 0), (0.0004, 0.012, 0.007), 0.0003, 1, "label_white")
    cross = L.box((0.0502, 0, 0), (0.0004, 0.009, 0.0022), 0.0002, 1, "label_blue")
    it.part("band").add(band, overlap, stitch, patch, mark, cross)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
