"""Phone (data/items.json "phone", docs/PHONE.md): a slab smartphone: aluminium frame with chamfered edges,
glass front (the screen part: dark glass, the engine draws the screen quad over it) and glass back, a
triple-lens camera bump with flash (camera_lens socket = the JSON lens), power and volume buttons, USB-C
port and speaker grilles, earpiece slot. The model shows no screen content. Procedural (path b).
Item space as the JSON camera block: body centre (-0.008, 0.085, 0.024), half (0.036, 0.075, 0.004)."""
import item_lib as L

C = (-0.008, 0.085, 0.024)
H = (0.036, 0.075, 0.004)


def build(ctx):
    it = L.Item("phone", "item")
    cx, cy, cz = C
    hx, hy, hz = H
    outline = [(x + cx, y + cy) for (x, y) in L.rounded_rect(hx * 2, hy * 2, 0.0085, 16)]
    frame = L._extrude_poly(outline, lambda p: L.Vector((p[0], p[1], cz - hz)), L.Vector((0, 0, hz * 2)), "aluminium_anodized", 0.0012, 3)
    inner = [(x + cx, y + cy) for (x, y) in L.rounded_rect(hx * 2 - 0.0016, hy * 2 - 0.0016, 0.0078, 16)]
    glass_f = L._extrude_poly(inner, lambda p: L.Vector((p[0], p[1], cz + hz - 0.0006)), L.Vector((0, 0, 0.0006)), "screen_glass", 0.0003, 1)
    glass_b = L._extrude_poly(inner, lambda p: L.Vector((p[0], p[1], cz - hz)), L.Vector((0, 0, 0.0006)), "glass_lens", 0.0003, 1)
    frame.cut(L._extrude_poly(inner, lambda p: L.Vector((p[0], p[1], cz + hz - 0.0006)), L.Vector((0, 0, 0.002)), "aluminium_anodized", 0, 0))
    frame.cut(L._extrude_poly(inner, lambda p: L.Vector((p[0], p[1], cz - hz - 0.002)), L.Vector((0, 0, 0.0026)), "aluminium_anodized", 0, 0))
    ear = L.box((cx, cy + hy - 0.004, cz + hz + 0.0001), (0.006, 0.0006, 0.0003), 0.0002, 1, "webbing_black")
    # Camera bump on the back: lens at the JSON lens default (body centre + (-hx + 0.012, hy - 0.014, -hz - 0.0005)).
    lx, ly = cx - hx + 0.012, cy + hy - 0.014
    bump = L._extrude_poly(L.rounded_rect(0.024, 0.024, 0.006, 6, lx + 0.002, ly - 0.002), lambda p: L.Vector((p[0], p[1], cz - hz - 0.0012)), L.Vector((0, 0, 0.0014)),
                           "glass_lens", 0.0004, 2)
    lenses = []
    for (dx, dy, r) in ((0.0, 0.0, 0.0042), (0.0, -0.0105, 0.0036), (0.0095, -0.005, 0.0032)):
        ring = L.ring_lathe([(r, 0), (r + 0.0011, 0), (r + 0.0011, 0.0013), (r, 0.0013)], (lx + dx, ly + dy, cz - hz - 0.0012), (0, 0, -1), 32, "aluminium_anodized")
        glass = L.cyl((lx + dx, ly + dy, cz - hz - 0.0016), (lx + dx, ly + dy, cz - hz - 0.0011), r, 32, "glass_lens", bevel=0.0001)
        lenses += [ring, glass]
    flash = L.cyl((lx + 0.0095, ly + 0.005, cz - hz - 0.0016), (lx + 0.0095, ly + 0.005, cz - hz - 0.0012), 0.0018, 16, "plastic_yellow", bevel=0)
    logo = L.text("DG", (cx, cy - 0.01, cz - hz - 0.0001), 0.008, (0, 0, -1), (0, 1, 0), 0.0001, "nickel")
    port = L.box((cx, cy - hy, cz), (0.0045, 0.0012, 0.0016), 0.0006, 1, "webbing_black")
    grille = [L.cyl((cx + sx * (0.009 + k * 0.0022), cy - hy - 0.0002, cz), (cx + sx * (0.009 + k * 0.0022), cy - hy + 0.0012, cz), 0.0006, 8, "webbing_black", bevel=0)
              for sx in (1, -1) for k in range(5)]
    sim = L.box((cx - hx - 0.0001, cy - 0.03, cz), (0.0003, 0.008, 0.0013), 0.0002, 1, "aluminium_anodized")
    sim_hole = L.cyl((cx - hx - 0.0006, cy - 0.024, cz), (cx - hx + 0.0004, cy - 0.024, cz), 0.0004, 8, "webbing_black", bevel=0)
    bands = [L.box((cx + sx * (hx + 0.00005), cy + dy, cz), (0.00025, 0.0006, hz * 0.9), 0, 0, "plastic_black") for sx in (1, -1) for dy in (hy - 0.012, -hy + 0.012)]
    bands += [L.box((cx, cy + sy * (hy + 0.00005), cz), (0.0006, 0.00025, hz * 0.9), 0, 0, "plastic_black").move((sx * (hx - 0.012), 0, 0)) for sy in (1, -1) for sx in (1, -1)]
    mics = [L.cyl((cx + 0.016 + k * 0.0018, cy + hy - 0.0002, cz), (cx + 0.016 + k * 0.0018, cy + hy + 0.0012, cz), 0.0004, 8, "webbing_black", bevel=0) for k in range(2)]
    it.part("body").add(frame, glass_b, ear, bump, flash, logo, port, *lenses, *grille, sim, sim_hole, *bands, *mics)
    it.part("screen").add(glass_f)
    btn = [L.box((cx + hx + 0.0004, cy + 0.03, cz), (0.0006, 0.009, 0.0012), 0.0004, 1, "aluminium_anodized"),
           L.box((cx - hx - 0.0004, cy + 0.04, cz), (0.0006, 0.007, 0.0012), 0.0004, 1, "aluminium_anodized"),
           L.box((cx - hx - 0.0004, cy + 0.023, cz), (0.0006, 0.007, 0.0012), 0.0004, 1, "aluminium_anodized")]
    it.part("buttons", mech="buttons").add(*btn)
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (-0.01, 0.1, 0))
    it.socket("screen", (cx, cy, cz + hz + 0.0005))
    it.socket("camera_lens", (lx, ly, cz - hz - 0.0005), (0, 180, 0))
    it.socket("front_lens", (cx, cy + hy - 0.0065 + 0.0035, cz + hz + 0.0005))
    return it
