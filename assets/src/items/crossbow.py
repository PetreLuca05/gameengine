"""Crossbow (data/weapons/04_crossbow.json): recurve tactical crossbow, cocked and loaded: aluminium
flight rail with a top Picatinny section, polymer riser with split limbs and a foot stirrup, the string
drawn to the latch, a carbon bolt with vanes and a field point on the rail, trigger box with an
ambidextrous safety, skeleton stock. Procedural (path b).

Fixed by the JSON: rail / bolt line y 0.05, muzzle (bolt tip) z -0.46, rail top 0.075 (optic at z 0),
sight line 0.093 (rear notch z -0.06, front post z -0.43), limbs at z -0.38 (+-0.22 m), support hand
(0, 0, -0.25)."""
import math

import item_lib as L

BORE = 0.05
RAIL_TOP = 0.075
RAIL_BASE = RAIL_TOP - 0.0094
LINE = 0.093
TIP = -0.46
LATCH_Z = -0.045


def rail():
    # Flight rail (barrel): an aluminium extrusion with the arrow groove on top, lightening slots.
    sec = [(-0.016, 0.026), (0.016, 0.026), (0.018, 0.034), (0.018, BORE - 0.003), (0.006, BORE - 0.003), (0.0035, BORE - 0.006), (-0.0035, BORE - 0.006),
           (-0.006, BORE - 0.003), (-0.018, BORE - 0.003), (-0.018, 0.034)]
    r = L.front_prism(sec, -0.39, 0.03, bevel=0.001, segs=2, mat="aluminium_anodized")
    for k in range(6):
        z = -0.33 + k * 0.05
        for sx in (1, -1):
            r.cut(L.box((sx * 0.018, 0.037, z), (0.003, 0.004, 0.017), 0, 0))
    top = L.picatinny(-0.06, 0.06, RAIL_BASE, mat="aluminium_anodized")
    riser_mount = L.box((0, RAIL_BASE - 0.008, 0.0), (0.012, 0.008, 0.062), 0.0012, 2, "aluminium_anodized")
    marks = [L.text("DEADGROUND  VX-185  185 lb", (-0.0182, 0.041, -0.2), 0.0045, (-1, 0, 0), (0, 1, 0), 0.0002, "paint_white")]
    return [r, top, riser_mount] + marks


def riser_and_limbs():
    out = []
    riser = L.box((0, 0.045, -0.38), (0.032, 0.018, 0.016), 0.004, 3, "polymer_matte")
    riser.cut(L.box((0, BORE - 0.002, -0.38), (0.006, 0.006, 0.03), 0, 0))
    out.append(riser)
    stirrup = L.top_prism([(-0.035, -0.395), (0.035, -0.395), (0.04, -0.43), (0.03, -0.475), (-0.03, -0.475), (-0.04, -0.43)], 0.032, 0.04, bevel=0.002, segs=2,
                          mat="polymer_matte")
    stirrup.cut(L.top_prism([(-0.025, -0.405), (0.025, -0.405), (0.028, -0.43), (0.022, -0.465), (-0.022, -0.465), (-0.028, -0.43)], 0.02, 0.06, 0, 0))
    out.append(stirrup)
    for sx in (1, -1):
        # Split limbs: two thin recurved blades from the riser out to the tips (bending back), with pockets.
        for dy in (-0.006, 0.006):
            pts = []
            n = 10
            for k in range(n + 1):
                t = k / n
                x = sx * (0.03 + t * 0.19)
                z = -0.38 + 0.065 * t * t - 0.012 * math.sin(math.pi * t)
                pts.append((x, z))
            blade = []
            for (x, z) in pts:
                blade.append((x, z - 0.012 + 0.006 * abs(x) / 0.22))
            for (x, z) in reversed(pts):
                blade.append((x, z + 0.004))
            b = L.top_prism(blade if sx < 0 else list(reversed(blade)), 0.054 + dy - 0.002, 0.054 + dy + 0.002, bevel=0.0008, segs=2, mat="polymer_od")
            out.append(b)
        pocket = L.box((sx * 0.036, 0.054, -0.381), (0.008, 0.012, 0.013), 0.002, 2, "aluminium_anodized")
        tip = L.cyl((sx * 0.22, 0.044, -0.315), (sx * 0.22, 0.064, -0.315), 0.0055, 16, "polymer_matte", bevel=0.0008)
        out += [pocket, tip]
    return out


def string():
    # Drawn string: tips -> latch, with the serving at the centre.
    out = []
    for sx in (1, -1):
        a = (sx * 0.22, 0.054, -0.315)
        b = (sx * 0.01, BORE + 0.001, LATCH_Z)
        out.append(L.cyl(a, b, 0.0011, 8, "webbing_black", bevel=0))
        out.append(L.cyl((sx * 0.22, 0.05, -0.315), (sx * 0.22, 0.058, -0.315), 0.0018, 8, "webbing_black", bevel=0))
    out.append(L.cyl((-0.012, BORE + 0.001, LATCH_Z), (0.012, BORE + 0.001, LATCH_Z), 0.0016, 10, "nickel", bevel=0))
    return out


def arrow():
    # Carbon bolt from the string to the tip: nock, shaft, three vanes, brass insert, field point.
    nz, tz = LATCH_Z, TIP
    y = BORE + 0.0045
    shaft = L.cyl((0, y, nz - 0.008), (0, y, tz + 0.02), 0.0041, 20, "polymer_glossy", bevel=0.0003)
    nock = L.lathe([(0, 0), (0.0035, 0), (0.0041, 0.004), (0.0041, 0.011), (0, 0.011)], (0, y, nz + 0.003), (0, 0, -1), 20, "plastic_orange")
    nock.cut(L.box((0, y, nz + 0.002), (0.005, 0.0012, 0.003), 0, 0))
    out = [shaft, nock]
    for k in range(3):
        a = math.radians(90 + k * 120)
        v = L.side_prism([(nz - 0.012, 0.0), (nz - 0.075, 0.0), (nz - 0.07, 0.009), (nz - 0.03, 0.011)], -0.0004, 0.0004, bevel=0.0002, segs=1,
                         mat="plastic_white" if k == 0 else "plastic_orange")
        v.move((0, 0.0041, 0))
        v.rotate((0, 0, 1), math.degrees(a) - 90, (0, 0, 0))
        v.move((0, y, 0))
        out.append(v)
    insert = L.cyl((0, y, tz + 0.02), (0, y, tz + 0.014), 0.0042, 20, "brass", bevel=0.0003)
    point = L.lathe([(0.0041, 0), (0.0041, 0.006), (0.003, 0.0115), (0, 0.014)], (0, y, tz + 0.014), (0, 0, -1), 20, "steel_bare")
    return out + [insert, point]


def trigger_box():
    box = L.side_prism([(-0.08, 0.026), (0.02, 0.026), (0.02, 0.006), (0.004, 0.002), (-0.012, -0.004), (-0.07, -0.004), (-0.08, 0.008)], -0.0135, 0.0135, bevel=0.0018,
                       segs=2, mat="polymer_matte")
    tg = L.side_prism([(-0.07, -0.003), (-0.068, -0.02), (-0.04, -0.0255), (-0.006, -0.0215), (0.004, -0.008), (0.0, -0.004), (-0.008, -0.016), (-0.04, -0.0205),
                       (-0.062, -0.016), (-0.064, -0.003)], -0.0055, 0.0055, bevel=0.0012, segs=2, mat="polymer_matte", angle=20)
    latch = L.box((0, BORE - 0.004, LATCH_Z + 0.012), (0.014, 0.008, 0.016), 0.002, 2, "aluminium_anodized")
    grip = L.side_prism([(-0.012, 0.004), (-0.004, -0.04), (0.0, -0.075), (0.032, -0.075), (0.03, -0.035), (0.024, 0.004)], -0.0145, 0.0145, bevel=0.0035, segs=3,
                        mat="polymer_matte", angle=20)
    grip.move((0, 0, 0.04))
    panel = lambda y, z: -0.068 < y < -0.004 and (0.03 + (-y) * 0.1) < z < (0.062 + (-y) * 0.1)
    for sx in (1, -1):
        grip.cut(L.stipple(sx * 0.0145, -0.07, 0.0, 0.02, 0.075, side=sx, inside=panel))
    # Foregrip under the rail (the support hand), finger ridges.
    fg = L.side_prism([(-0.29, 0.027), (-0.2, 0.027), (-0.205, 0.006), (-0.215, 0.002), (-0.275, 0.002), (-0.285, 0.008)], -0.0175, 0.0175, bevel=0.003, segs=3,
                      mat="polymer_matte")
    fg.cut(L.merge(*[L.box((0, 0.002, -0.27 + k * 0.012), (0.02, 0.0015, 0.0025), 0, 0) for k in range(5)]))
    return [box, tg, latch, grip, fg]


def quiver():
    # Side quiver on the left: a hood holding three spare bolts point-down, a bracket to the rail.
    out = []
    hood = L.box((-0.05, 0.03, -0.39), (0.022, 0.016, 0.022), 0.004, 3, "polymer_matte")
    hood.cut(L.box((-0.05, 0.03, -0.37), (0.017, 0.012, 0.022), 0, 0))
    clip = L.box((-0.05, 0.03, -0.14), (0.02, 0.006, 0.008), 0.002, 2, "polymer_matte")
    arm = L.box((-0.03, 0.03, -0.25), (0.006, 0.004, 0.006), 0.0012, 2, "aluminium_anodized")
    out += [hood, clip, arm]
    for k, dx in enumerate((-0.012, 0.0, 0.012)):
        x = -0.05 + dx
        sh = L.cyl((x, 0.03, -0.38), (x, 0.03, -0.0), 0.0041, 20, "polymer_glossy", bevel=0.0003)
        nk = L.lathe([(0, 0), (0.0035, 0), (0.0041, 0.004), (0.0041, 0.011), (0, 0.011)], (x, 0.03, 0.011), (0, 0, -1), 20, "plastic_orange")
        vane = L.side_prism([(-0.012, 0.0), (-0.07, 0.0), (-0.066, 0.009), (-0.03, 0.011)], -0.0004, 0.0004, 0.0002, 1, "plastic_white")
        vane.move((x, 0.0341, 0.0))
        out += [sh, nk, vane]
    return out


def stock():
    z0, z1, y = 0.02, 0.32, 0.03
    pts = [(z0, y + 0.03), (z1 - 0.01, y + 0.036), (z1, y + 0.04), (z1, y - 0.075), (z1 - 0.03, y - 0.075), (0.11, y - 0.02), (z0, y - 0.004)]
    s = L.side_prism(pts, -0.0175, 0.0175, bevel=0.003, segs=3, mat="polymer_matte", angle=20)
    s.cut(L.side_prism([(z0 + 0.12, y + 0.022), (z1 - 0.035, y + 0.026), (z1 - 0.035, y - 0.05), (0.15, y - 0.016)], -0.03, 0.03, 0, 0))
    pad = L.side_prism([(z1, y + 0.041), (z1 + 0.014, y + 0.039), (z1 + 0.015, y - 0.073), (z1, y - 0.076)], -0.0185, 0.0185, bevel=0.003, segs=3, mat="rubber")
    pad.cut(L.merge(*[L.box((0, y + 0.03 - k * 0.0085, z1 + 0.015), (0.021, 0.0012, 0.0012), 0, 0) for k in range(12)]))
    return [s, pad]


def sights():
    rear = L.front_prism([(-0.009, RAIL_TOP - 0.0005), (0.009, RAIL_TOP - 0.0005), (0.008, LINE), (0.00225, LINE), (0.00225, LINE - 0.004), (-0.00225, LINE - 0.004),
                          (-0.00225, LINE), (-0.008, LINE)], -0.064, -0.056, bevel=0.0005, segs=2, mat="steel_dark")
    rbase = L.box((0, RAIL_TOP + 0.0015, -0.06), (0.0108, 0.0018, 0.0075), 0.0006, 2, "steel_dark")
    front = L.box((0, (0.038 + LINE) * 0.5, -0.43), (0.0015, (LINE - 0.038) * 0.5, 0.0015), 0.0003, 1, "steel_dark")
    fbase = L.box((0, 0.034, -0.43), (0.006, 0.006, 0.008), 0.001, 2, "polymer_matte")
    return [rear, rbase], [front, fbase]


def build(ctx):
    it = L.Item("crossbow", "weapon")
    it.part("rail").add(*rail())
    it.part("riser_limbs").add(*riser_and_limbs())
    it.part("string", mech="string").add(*string())
    it.part("bolt", mech="projectile").add(*arrow())
    it.part("trigger_box").add(*trigger_box())
    it.part("trigger", pivot=(0, 0.012, -0.03), mech="trigger").add(L.side_prism([(-0.033, 0.014), (-0.026, 0.014), (-0.027, -0.01), (-0.034, -0.012), (-0.036, -0.01),
                                                                                    (-0.032, 0.0)], -0.003, 0.003, 0.0008, 2, "steel_dark"))
    sf = L.box((0, BORE + 0.006, 0.006), (0.016, 0.003, 0.006), 0.0012, 2, "polymer_matte")
    it.part("safety", pivot=(0, BORE + 0.006, 0.006), mech="safety").add(sf, L.text("S", (0.0161, BORE + 0.006, 0.006), 0.003, (1, 0, 0), (0, 1, 0), 0.0002, "paint_white"))
    it.part("stock").add(*stock())
    it.part("quiver", detail=True).add(*quiver())
    rear, front = sights()
    it.part("rear_sight", hide="optic").add(*rear)
    it.part("front_sight", hide="optic").add(*front)
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (0, 0.0, -0.25))
    it.socket("sight", (0, LINE, -0.06))
    it.socket("muzzle", (0, BORE, TIP))
    it.socket("optic", (0, RAIL_TOP, 0.0))
    it.socket("string_latch", (0, BORE + 0.001, LATCH_Z))
    it.socket("bolt_nock", (0, BORE + 0.0045, LATCH_Z))
    return it
