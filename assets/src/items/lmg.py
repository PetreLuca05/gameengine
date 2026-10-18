"""LMG (data/weapons/08_lmg.json): belt-fed squad automatic weapon (M249 pattern): riveted stamped receiver,
hinged feed cover with a top rail, feed tray, belt box on the right with the linked belt running into the
tray, heavy barrel with a carrying handle and heat shield, gas regulator, folded bipod, ribbed polymer
handguard, skeleton stock. Procedural (path b).

Fixed by the JSON: bore y 0.05, muzzle z -0.64, rail top 0.09 (optic at z 0), sight line 0.14 (rear z 0.02,
front z -0.58), support hand / underbarrel (0, 0.02 / 0.01, -0.34), stock (0, 0.035, 0.09)."""
import math

import _parts as P
import item_lib as L

BORE = 0.05
RAIL_TOP = 0.09
RAIL_BASE = RAIL_TOP - 0.0094
LINE = 0.14
MUZZLE = -0.64
RZ0, RZ1 = -0.14, 0.09      # receiver


def receiver():
    hw = 0.0215
    rec = L.box((0, 0.047, (RZ0 + RZ1) * 0.5), (hw, 0.027, (RZ1 - RZ0) * 0.5), 0.0015, 2, "steel_parkerized")
    # Side stiffening ribs, ejection port underneath, charging handle slot on the right.
    for sx in (1, -1):
        rec.union(L.box((sx * (hw + 0.0006), 0.061, (RZ0 + RZ1) * 0.5 - 0.01), (0.0012, 0.0035, (RZ1 - RZ0) * 0.5 - 0.02), 0.0006, 1, "steel_parkerized"))
    rec.cut(L.box((0, 0.02, -0.045), (0.009, 0.004, 0.028), 0, 0))
    rec.cut(L.box((hw, 0.045, -0.06), (0.004, 0.0035, 0.06), 0, 0))
    rivets = []
    for sx in (1, -1):
        for z in [RZ0 + 0.012 + k * 0.022 for k in range(10)]:
            for y in (0.026, 0.048, 0.07):
                rivets.append(L.lathe([(0, 0), (0.0018, 0), (0.0016, 0.0006), (0, 0.0009)], (sx * hw, y, z), (sx, 0, 0), 10, "steel_parkerized"))
    trig_house = L.side_prism([(-0.075, 0.021), (0.05, 0.021), (0.05, 0.006), (0.032, 0.002), (-0.004, 0.002), (-0.012, -0.004), (-0.06, -0.004), (-0.075, 0.008)],
                              -0.0125, 0.0125, bevel=0.0012, segs=2, mat="polymer_matte")
    tg = L.side_prism([(-0.06, -0.003), (-0.058, -0.02), (-0.03, -0.0255), (-0.006, -0.0215), (0.004, -0.008), (0.0, -0.004), (-0.008, -0.016), (-0.03, -0.0205),
                       (-0.052, -0.016), (-0.054, -0.003)], -0.0055, 0.0055, bevel=0.0012, segs=2, mat="polymer_matte", angle=20)
    marks = [L.text("DG249  5.56mm", (-hw - 0.0006, 0.048, -0.02), 0.005, (-1, 0, 0), (0, 1, 0), 0.0002),
             L.text("SN 7741902", (-hw - 0.0006, 0.04, -0.02), 0.0032, (-1, 0, 0), (0, 1, 0), 0.00015)]
    # Belt feed opening on the right (the belt enters here), feed tray lip.
    rec.cut(L.box((hw, 0.068, -0.06), (0.006, 0.0062, 0.02), 0, 0))
    tray = L.box((hw + 0.006, 0.0675, -0.06), (0.006, 0.0012, 0.022), 0.0004, 1, "steel_dark")
    return [rec, trig_house, tg, tray] + rivets + marks


def feed_cover():
    # Hinged at its rear (pivot), rail on top, latch at the back, rear sight leaf.
    hw = 0.0225
    cover = L.side_prism([(RZ0 + 0.065, 0.074), (RZ1 - 0.005, 0.074), (RZ1, 0.077), (RZ1, RAIL_BASE), (RZ0 + 0.07, RAIL_BASE), (RZ0 + 0.062, 0.077)], -hw, hw,
                         bevel=0.0012, segs=2, mat="steel_parkerized")
    for sx in (1, -1):
        cover.cut(L.box((sx * hw, 0.0775, -0.02), (0.0015, 0.0018, 0.05), 0, 0))
    rail = L.picatinny(-0.07, 0.07, RAIL_BASE, mat="steel_parkerized")
    latch = L.box((0, RAIL_BASE - 0.002, RZ1 + 0.006), (0.012, 0.004, 0.006), 0.0012, 2, "steel_parkerized")
    latch.cut(L.serrations(RZ1 + 0.002, RZ1 + 0.011, RAIL_BASE - 0.005, RAIL_BASE + 0.001, 0.012, 0.0012, 0.0003))
    hinge = L.cyl((-hw - 0.001, 0.076, RZ1 - 0.004), (hw + 0.001, 0.076, RZ1 - 0.004), 0.003, 16, "steel_dark", bevel=0.0004)
    return [cover, rail, latch, hinge]


def rear_sight():
    # Tall folding leaf with elevation drum (the sight line is 5 cm above the rail).
    base = L.box((0, RAIL_TOP + 0.004, 0.02), (0.0115, 0.004, 0.012), 0.0008, 2, "steel_parkerized")
    leaf = L.front_prism([(-0.008, RAIL_TOP + 0.006), (0.008, RAIL_TOP + 0.006), (0.006, LINE + 0.006), (-0.006, LINE + 0.006)], 0.018, 0.022, bevel=0.0006,
                         segs=2, mat="steel_parkerized")
    leaf.cut(L.cyl((0, LINE, 0.015), (0, LINE, 0.025), 0.0017, 16, bevel=0))
    drum = L.cyl((0.0115, RAIL_TOP + 0.012, 0.022), (0.019, RAIL_TOP + 0.012, 0.022), 0.0065, 24, "steel_parkerized", bevel=0.0005)
    drum.cut(L.knurl((0.012, RAIL_TOP + 0.012, 0.022), (0.019, RAIL_TOP + 0.012, 0.022), 0.0065, 24, 0.0004))
    return [base, leaf, drum]


def barrel():
    prof = [(0, 0), (0.0145, 0), (0.0145, 0.02), (0.0118, 0.025), (0.0118, 0.38), (0.011, 0.385), (0.011, 0.452), (0.0095, 0.455), (0.0095, -MUZZLE + RZ0 - 0.04),
            (0.0072, -MUZZLE + RZ0 - 0.04), (0.0072, -MUZZLE + RZ0), (0, -MUZZLE + RZ0)]
    b = L.lathe(prof, (0, BORE, RZ0), (0, 0, -1), 40, "steel_parkerized")
    b.cut(L.cyl((0, BORE, MUZZLE - 0.003), (0, BORE, MUZZLE + 0.012), 0.003, 20, bevel=0))
    # Barrel extension collar + quick-change handle base, heat shield (perforated) on top.
    collar = L.cyl((0, BORE, RZ0 - 0.002), (0, BORE, RZ0 - 0.02), 0.017, 40, "steel_parkerized", bevel=0.001)
    shield = L.front_prism([(-0.012, BORE + 0.006), (0.012, BORE + 0.006), (0.0145, BORE + 0.014), (0.009, BORE + 0.0185), (-0.009, BORE + 0.0185), (-0.0145, BORE + 0.014)],
                           -0.42, -0.25, bevel=0.0008, segs=2, mat="steel_parkerized")
    shield.cut(L.array(L.cyl((0, BORE + 0.022, -0.405), (0, BORE + 0.01, -0.405), 0.003, 12, bevel=0), 8, (0, 0, 0.02)))
    gas = L.box((0, BORE - 0.012, -0.44), (0.0105, 0.016, 0.014), 0.0015, 2, "steel_parkerized")
    reg = L.cyl((-0.0105, BORE - 0.022, -0.44), (0.0125, BORE - 0.022, -0.44), 0.0072, 24, "steel_parkerized", bevel=0.0006)
    for k in range(3):   # regulator positions
        reg.cut(L.box((0.0125, BORE - 0.022 + 0.0072 * math.cos(k * 1.2), -0.44 + 0.0072 * math.sin(k * 1.2)), (0.002, 0.0012, 0.0012), 0, 0))
    tube = L.cyl((0, BORE - 0.022, -0.44), (0, BORE - 0.022, RZ0), 0.0062, 24, "steel_parkerized", bevel=0.0005)
    return [b, collar, shield, gas, reg, tube]


def carry_handle():
    # Folding carry handle over the barrel (polymer grip on a steel arm).
    arm = L.side_prism([(RZ0 - 0.02, BORE + 0.017), (RZ0 - 0.006, BORE + 0.017), (RZ0 - 0.012, BORE + 0.06), (RZ0 - 0.03, BORE + 0.064)], -0.004, 0.004, bevel=0.001,
                       segs=2, mat="steel_parkerized")
    grip = L.cyl((0, BORE + 0.064, RZ0 - 0.02), (0, BORE + 0.064, RZ0 - 0.115), 0.0095, 24, "polymer_matte", bevel=0.0015)
    grip.cut(L.merge(*[L.torus((0, BORE + 0.064, RZ0 - 0.035 - k * 0.012), (0, 0, -1), 0.0098, 0.0012, 24, 6) for k in range(7)]))
    arm2 = L.side_prism([(RZ0 - 0.12, BORE + 0.062), (RZ0 - 0.108, BORE + 0.062), (RZ0 - 0.1, BORE + 0.02), (RZ0 - 0.112, BORE + 0.018)], -0.004, 0.004, bevel=0.001,
                        segs=2, mat="steel_parkerized")
    return [arm, grip, arm2]


def front_sight():
    z = -0.58
    base = L.cyl((0, BORE, z + 0.01), (0, BORE, z - 0.012), 0.0125, 32, "steel_parkerized", bevel=0.0008)
    tower = L.front_prism([(-0.006, BORE + 0.008), (0.006, BORE + 0.008), (0.004, LINE - 0.012), (-0.004, LINE - 0.012)], z - 0.004, z + 0.004, bevel=0.0008, segs=2,
                          mat="steel_parkerized")
    post = L.box((0, LINE - 0.006, z), (0.0014, 0.006, 0.0014), 0.0003, 1, "steel_parkerized")
    ears = [L.box((sx * 0.0075, LINE - 0.006, z), (0.0012, 0.009, 0.004), 0.0004, 1, "steel_parkerized") for sx in (1, -1)]
    return [base, tower, post] + ears


def flash_hider():
    prof = [(0.0076, 0.0), (0.0115, 0.0), (0.0115, 0.042), (0.0105, 0.045), (0.0068, 0.045), (0.0068, 0.028), (0.0076, 0.026)]
    fh = L.ring_lathe(prof, (0, BORE, MUZZLE + 0.045), (0, 0, -1), 40, "steel_parkerized", bevel=0.0004)
    cuts = []
    for k in range(4):
        a = k * math.pi / 2 + math.pi / 4
        c = L.box((0, 0, 0), (0.0016, 0.012, 0.014), 0, 0)
        c.rotate((0, 0, 1), math.degrees(a) - 90)
        c.move((math.cos(a) * 0.009, BORE + math.sin(a) * 0.009, MUZZLE + 0.014))
        cuts.append(c)
    fh.cut(L.merge(*cuts))
    return [fh]


def handguard():
    z0, z1 = RZ0 - 0.022, -0.36
    sec = [(-0.026, 0.012), (0.026, 0.012), (0.03, 0.022), (0.03, 0.04), (0.024, 0.048), (-0.024, 0.048), (-0.03, 0.04), (-0.03, 0.022)]
    hg = L.front_prism(sec, z1, z0, bevel=0.003, segs=3, mat="polymer_matte")
    hg.cut(L.cyl((0, BORE - 0.022, z1 - 0.01), (0, BORE - 0.022, z0 + 0.01), 0.0085, 24, bevel=0))
    ribs = []
    for k in range(13):
        z = z1 + 0.012 + k * 0.016
        for sx in (1, -1):
            ribs.append(L.box((sx * 0.03, 0.03, z), (0.0012, 0.009, 0.0025), 0, 0))
        ribs.append(L.box((0, 0.012, z), (0.018, 0.0012, 0.0025), 0, 0))
    hg.cut(L.merge(*ribs))
    rail = L.picatinny(-0.37, -0.31, 0.012, mat="steel_parkerized")
    rail.rotate((0, 0, 1), 180, (0, 0.012, 0))
    return [hg, rail]


def bipod():
    # Folded under the barrel: yoke at the gas block, legs along the handguard, feet with spikes.
    yoke = L.box((0, BORE - 0.035, -0.46), (0.014, 0.006, 0.01), 0.0015, 2, "steel_parkerized")
    out = [yoke]
    for sx in (1, -1):
        leg = L.cyl((sx * 0.011, BORE - 0.038, -0.465), (sx * 0.014, 0.004, -0.25), 0.0052, 20, "steel_parkerized", bevel=0.0006)
        inner = L.cyl((sx * 0.014, 0.004, -0.25), (sx * 0.0145, 0.0, -0.215), 0.0042, 16, "steel_parkerized", bevel=0.0005)
        foot = L.cyl((sx * 0.0145, 0.0, -0.215), (sx * 0.0145, -0.002, -0.2), 0.006, 16, "rubber", bevel=0.001)
        out += [leg, inner, foot]
    return out


def belt_box():
    # Polymer 200-round box (right side, below the feed), latch, the belt curling out of its top.
    hw, cx = 0.029, 0.052
    box = L.box((cx, -0.005, -0.05), (hw, 0.062, 0.06), 0.004, 3, "polymer_od")
    box.cut(L.merge(*[L.box((cx + hw, -0.04 + k * 0.014, -0.05), (0.0012, 0.004, 0.05), 0, 0) for k in range(5)]))
    lid = L.box((cx, 0.0585, -0.05), (hw + 0.001, 0.0035, 0.061), 0.002, 2, "polymer_od")
    latch = L.box((cx + hw + 0.002, 0.045, -0.05), (0.002, 0.012, 0.01), 0.0008, 2, "steel_dark")
    mark = L.text("5.56 LINKED 200", (cx + hw + 0.0002, -0.01, -0.05), 0.0055, (1, 0, 0), (0, 1, 0), 0.00025, "paint_yellow")
    out = [box, lid, latch, mark]
    # Linked rounds: an arc from the box opening up into the feed tray (x 0.022 .. 0.03, y 0.06 .. 0.068).
    n = 12
    for k in range(n):
        t = k / (n - 1)
        x = 0.03 + (cx - 0.012 - 0.03) * (1 - t) ** 1.5
        y = 0.068 - (0.068 - 0.062) * t * t
        z = -0.078 + t * 0.054
        link = L.ring_lathe([(0.0049, 0), (0.0058, 0), (0.0058, 0.006), (0.0049, 0.006)], (x - 0.003, y, z), (1, 0, 0), 16, "steel_dark")
        case = L.lathe([(0, 0), (0.0046, 0), (0.0048, 0.0012), (0.0048, 0.034), (0.0044, 0.037), (0.0031, 0.039), (0.0029, 0.0445)], (x + 0.016, y, z), (-1, 0, 0), 16,
                       "brass")
        tip = L.lathe([(0.0028, 0), (0.0028, 0.006), (0.0024, 0.012), (0.0012, 0.017), (0, 0.0195)], (x + 0.016 - 0.0445, y, z), (-1, 0, 0), 16, "copper")
        out += [link, case, tip]
    return out


def charging_handle():
    arm = L.box((0.026, 0.045, -0.06), (0.0035, 0.004, 0.018), 0.0008, 2, "steel_parkerized")
    knob = L.top_prism([(0.026, -0.08), (0.045, -0.078), (0.047, -0.07), (0.028, -0.068)], 0.04, 0.05, bevel=0.0015, segs=2, mat="steel_parkerized")
    return [arm, knob]


def stock():
    z0, z1, y = RZ1, 0.355, 0.035
    pts = [(z0, y + 0.03), (z0 + 0.04, y + 0.03), (z1 - 0.01, y + 0.036), (z1, y + 0.04), (z1, y - 0.07), (z1 - 0.03, y - 0.07), (z0 + 0.08, y - 0.035), (z0, y - 0.022)]
    s = L.side_prism(pts, -0.019, 0.019, bevel=0.003, segs=3, mat="polymer_matte", angle=20)
    s.cut(L.side_prism([(z0 + 0.04, y + 0.02), (z1 - 0.035, y + 0.024), (z1 - 0.035, y - 0.044), (z0 + 0.09, y - 0.02)], -0.03, 0.03, 0, 0))
    s.cut(L.side_prism([(z1 - 0.025, y + 0.026), (z1 - 0.012, y + 0.028), (z1 - 0.012, y - 0.06), (z1 - 0.025, y - 0.058)], -0.03, 0.03, 0, 0))
    pad = L.side_prism([(z1, y + 0.041), (z1 + 0.014, y + 0.039), (z1 + 0.015, y - 0.068), (z1, y - 0.071)], -0.0198, 0.0198, bevel=0.003, segs=3, mat="rubber")
    pad.cut(L.merge(*[L.box((0, y + 0.03 - k * 0.0085, z1 + 0.015), (0.021, 0.0012, 0.0012), 0, 0) for k in range(12)]))
    buffer = L.cyl((0, y + 0.012, z0 - 0.002), (0, y + 0.012, z0 + 0.03), 0.014, 32, "steel_parkerized", bevel=0.0008)
    sling = L.tube((0.019, y - 0.05, z1 - 0.045), (0.023, y - 0.05, z1 - 0.045), 0.0058, 0.0039, 20, "steel_parkerized")
    return [s, pad, buffer, sling]


def build(ctx):
    it = L.Item("lmg", "weapon")
    it.part("receiver").add(*receiver())
    it.part("feed_cover", pivot=(0, 0.076, RZ1 - 0.004), mech="feed_cover").add(*feed_cover())
    it.part("rear_sight", hide="optic").add(*rear_sight())
    g, cap = P.ar_pistol_grip()
    it.part("pistol_grip").add(g, cap)
    it.part("trigger", pivot=(0, 0.012, -0.027), mech="trigger").add(P.ar_trigger((0, 0.012, -0.027)))
    it.part("barrel").add(*barrel())
    it.part("carry_handle", detail=False).add(*carry_handle())
    it.part("front_sight", hide="optic").add(*front_sight())
    it.part("muzzle_device", hide="muzzle").add(*flash_hider())
    it.part("handguard").add(*handguard())
    it.part("bipod", mech="bipod").add(*bipod())
    it.part("belt_box", pivot=(0.035, 0.0, -0.05), mech="mag").add(*belt_box())
    it.part("charging_handle", pivot=(0.027, 0.045, -0.06), mech="bolt").add(*charging_handle())
    sf = L.cyl((-0.0125, 0.012, 0.012), (0.0145, 0.012, 0.012), 0.0035, 16, "steel_dark", bevel=0.0005)
    it.part("safety", pivot=(0, 0.012, 0.012), mech="safety").add(sf, L.cyl((0.0145, 0.012, 0.012), (0.0152, 0.012, 0.012), 0.0018, 12, "paint_red", bevel=0))
    it.part("stock", hide="stock").add(*stock())
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (0, 0.02, -0.34))
    it.socket("sight", (0, LINE, 0.02))
    it.socket("muzzle", (0, BORE, MUZZLE))
    it.socket("optic", (0, RAIL_TOP, 0.0))
    it.socket("underbarrel", (0, 0.01, -0.34))
    it.socket("stock", (0, 0.035, 0.09))
    it.socket("ejection_port", (0, 0.02, -0.045), (90, 0, 0))
    it.socket("shell_eject", (0, 0.016, -0.04), (100, 0, 0))
    it.socket("feed_tray", (0.028, 0.0675, -0.06))
    it.socket("charging_handle", (0.046, 0.045, -0.074))
    return it
