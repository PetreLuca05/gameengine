"""Combat knife (data/weapons/06_knife.json): 16 cm clip-point blade (spine on +Z, edge on -Z) with a
fuller and a ground bevel, steel crossguard, stacked-ring handle (rubber / leather look), steel pommel
with a lanyard hole. Procedural (path b). Melee item space: the blade runs along +Y from the grip."""
import item_lib as L

Y0 = 0.032   # blade base (the guard's front face)
LEN = 0.16


def blade():
    # Plan (y, z) outline extruded across x: the clip point drops towards the edge.
    y1 = Y0 + LEN
    pts = [(Y0, -0.0145), (Y0 + 0.11, -0.0142), (y1 - 0.025, -0.011), (y1 - 0.007, -0.004), (y1, 0.0015), (y1 - 0.012, 0.007), (y1 - 0.045, 0.0115), (Y0 + 0.07, 0.0145),
           (Y0, 0.0145)]
    # Side-profile in the (z, y) plane for side_prism: x is the blade's thickness.
    zy = [(z, y) for (y, z) in pts]
    b = L.side_prism(zy, -0.0028, 0.0028, bevel=0.0004, segs=2, mat="steel_parkerized", angle=25)
    # Edge grind: the flats are cut back near the edge on both sides (a bare-steel bevel strip remains).
    for sx in (1, -1):
        b.cut(L.side_prism([(-0.0146, Y0 + 0.006), (-0.0146, y1 + 0.01), (-0.008, y1 + 0.01), (-0.008, Y0 + 0.006)], sx * 0.0019, sx * 0.0035, 0, 0))
    # Fuller: a groove along the flat on both sides.
    for sx in (1, -1):
        b.cut(L.cyl((sx * 0.0036, Y0 + 0.012, 0.004), (sx * 0.0036, Y0 + 0.085, 0.004), 0.0022, 16, bevel=0))
    edge = L.side_prism([(-0.0146, Y0 + 0.006), (-0.0146, y1 - 0.02), (-0.008, y1 - 0.02), (-0.008, Y0 + 0.006)], -0.00185, 0.00185, bevel=0.0002, segs=1,
                        mat="steel_bare")
    mark = L.text("DG USMC", (0.0029, Y0 + 0.03, 0.002), 0.0032, (1, 0, 0), (0, 0, 1), 0.00012, "engrave")
    return [b, edge, mark]


def handle():
    guard = L.top_prism(L.rounded_rect(0.012, 0.05, 0.003, 3), Y0 - 0.006, Y0, bevel=0.0012, segs=2, mat="steel_parkerized")
    rings = []
    n = 14
    for k in range(n):
        y = Y0 - 0.008 - k * 0.0052
        bulge = 1.0 + 0.07 * (1 - abs((k - n * 0.55) / (n * 0.5)) ** 2)
        r = L.lathe([(0.0, 0.0), (0.0108 * bulge, 0.0), (0.0114 * bulge, 0.0008), (0.0114 * bulge, 0.0044), (0.0108 * bulge, 0.0052), (0.0, 0.0052)], (0, y, 0), (0, -1, 0),
                    28, "rubber" if k % 2 else "fabric_canvas")
        r.scale((0.82, 1.0, 1.0), (0, y, 0))
        rings.append(r)
    pommel = L.lathe([(0.0, 0.0), (0.0105, 0.0), (0.0118, 0.004), (0.0112, 0.011), (0.0085, 0.016), (0.0, 0.017)], (0, Y0 - 0.008 - n * 0.0052, 0), (0, -1, 0), 32,
                     "steel_parkerized")
    pommel.scale((0.85, 1.0, 1.0), (0, 0, 0))
    pommel.cut(L.cyl((-0.02, Y0 - 0.008 - n * 0.0052 - 0.011, 0), (0.02, Y0 - 0.008 - n * 0.0052 - 0.011, 0), 0.0022, 12, bevel=0))
    return [guard, pommel] + rings


def build(ctx):
    it = L.Item("knife", "weapon")
    it.part("blade").add(*blade())
    it.part("handle").add(*handle())
    it.socket("grip_r", (0, 0, 0))
    it.socket("tip", (0, Y0 + LEN, 0.0015))
    it.socket("edge", (0, Y0 + 0.08, -0.0145))
    return it
