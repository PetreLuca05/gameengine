"""Bolt sniper (data/weapons/10_sniper.json): long-action bolt rifle in a walnut sporter-target stock
(checkered grip / forend, cheek piece, recoil pad, sling studs), round receiver with a Picatinny scope
base, three-lug bolt with a swept handle, hinged floorplate, heavy sporter barrel with a banded front
sight, flip-up rear aperture on the base. Procedural (path b).

Fixed by the JSON: bore y 0.05, muzzle z -0.83, rail top 0.072 (optic at z -0.04), sight line 0.098
(rear z 0, front z -0.78), support hand (0, 0.02, -0.36), stock slot (0, 0.02, 0.08)."""
import math

import _parts as P
import item_lib as L

BORE = 0.05
RAIL_TOP = 0.072
RAIL_BASE = RAIL_TOP - 0.0094
LINE = 0.098
MUZZLE = -0.83
RX0, RX1 = -0.17, 0.055   # receiver (front ring .. tang)


def receiver():
    r = 0.0148
    rec = L.cyl((0, BORE, RX0), (0, BORE, RX1 - 0.012), r, 48, "steel_blued", bevel=0.0012)
    rec.cut(L.box((r * 0.8, BORE + 0.004, -0.045), (0.008, 0.009, 0.042), 0, 0))           # ejection port
    rec.cut(L.box((0.0, BORE + r, -0.002), (0.0045, 0.006, 0.05), 0, 0))                   # bolt raceway slot (top rear)
    tang = L.side_prism([(RX1 - 0.016, BORE - 0.012), (RX1 + 0.01, BORE - 0.012), (RX1 + 0.03, BORE - 0.02), (RX1 + 0.026, BORE - 0.016), (RX1 - 0.016, BORE + 0.008)],
                        -0.0075, 0.0075, bevel=0.0012, segs=2, mat="steel_blued")
    lug = L.box((0, BORE - r - 0.003, RX0 + 0.012), (0.009, 0.004, 0.006), 0.0008, 2, "steel_blued")
    base = L.box((0, RAIL_BASE - 0.002, -0.06), (0.0105, 0.0025, 0.105), 0.0006, 1, "steel_blued")
    rail = L.picatinny(-0.165, 0.045, RAIL_BASE, mat="steel_blued")
    screws = [L.screw((0, RAIL_BASE + 0.0062, z), (0, 1, 0), 0.0024, 0.0006, slot=False, hexsock=True) for z in (-0.145, -0.115, 0.005, 0.03)]
    marks = [L.text("DEADGROUND  M-70  .308 WIN", (-r + 0.0006, BORE + 0.004, -0.09), 0.0034, (-1, 0, 0.0), (0, 1, 0), 0.0002)]
    return [rec, tang, lug, base, rail] + screws + marks


def bolt():
    # Bolt body in the receiver, three lugs at the head, swept handle on the right with a knurled knob.
    b = L.cyl((0, BORE, -0.105), (0, BORE, 0.07), 0.0095, 36, "steel_bare", bevel=0.0008)
    b.cut(L.merge(*[L.torus((0, BORE, z), (0, 0, 1), 0.0095, 0.0006, 36, 6) for z in (-0.06, -0.05, -0.04)]))   # spiral flutes (rings)
    shroud = L.lathe([(0, 0), (0.0102, 0), (0.0108, 0.004), (0.0108, 0.022), (0.009, 0.03), (0.004, 0.034), (0, 0.034)], (0, BORE, 0.07), (0, 0, 1), 36, "steel_blued")
    lugs = [L.box((math.cos(a) * 0.0095, BORE + math.sin(a) * 0.0095, -0.1), (0.0022, 0.0022, 0.005), 0.0004, 1, "steel_bare") for a in (0.0, 2.1, 4.2)]
    root = Lp = (0.009, BORE + 0.004, 0.035)
    handle = L.cyl(root, (0.038, BORE + 0.0045, 0.042), 0.0032, 20, "steel_blued", bevel=0.0005)
    knob = L.lathe([(0, 0), (0.0065, 0.002), (0.0085, 0.008), (0.0088, 0.014), (0.0075, 0.019), (0, 0.021)], (0.034, BORE + 0.0045, 0.041), (1, 0.02, 0.2), 28,
                   "steel_blued")
    knob.cut(L.knurl((0.039, BORE + 0.0045, 0.042), (0.052, BORE + 0.0047, 0.0446), 0.0088, 24, 0.0005))
    return [b, shroud, handle, knob] + lugs


def barrel():
    z0 = RX0
    Lb = z0 - MUZZLE
    prof = [(0, 0), (0.0148, 0), (0.0148, 0.022), (0.0135, 0.03), (0.0118, 0.2), (0.0098, 0.48), (0.0092, Lb - 0.004), (0.0074, Lb), (0, Lb)]
    b = L.lathe(prof, (0, BORE, z0), (0, 0, -1), 48, "steel_blued")
    b.cut(L.cyl((0, BORE, MUZZLE - 0.003), (0, BORE, MUZZLE + 0.006), 0.0035, 20, bevel=0))
    recoil_lug = L.box((0, BORE - 0.016, z0 - 0.002), (0.0095, 0.006, 0.0025), 0.0005, 1, "steel_blued")
    return [b, recoil_lug]


def front_sight():
    z = -0.78
    band = L.cyl((0, BORE, z + 0.007), (0, BORE, z - 0.007), 0.0112, 32, "steel_blued", bevel=0.0008)
    ramp = L.side_prism([(z - 0.009, BORE + 0.009), (z + 0.012, BORE + 0.009), (z + 0.004, BORE + 0.02), (z - 0.006, BORE + 0.02)], -0.0035, 0.0035, bevel=0.0008,
                        segs=2, mat="steel_blued")
    hood = L.ring_lathe([(0.0062, 0), (0.0072, 0), (0.0072, 0.016), (0.0062, 0.016)], (0, LINE - 0.002, z + 0.008), (0, 0, -1), 24, "steel_blued")
    post = L.front_prism([(-0.001, BORE + 0.019), (0.001, BORE + 0.019), (0.0008, LINE), (-0.0008, LINE)], z - 0.001, z + 0.001, 0.0002, 1, "steel_blued")
    hood_leg = L.box((0, BORE + 0.022, z), (0.0028, 0.004, 0.006), 0.0005, 1, "steel_blued")
    return [band, ramp], [hood, hood_leg, post]   # the post reaches the sight line: it hides with the hood under an optic


def stock():
    # Walnut sporter / target stock, side profile (z, y), with a raised comb and cheek piece on the left.
    pts = [(-0.4, BORE - 0.01), (-0.395, BORE - 0.026), (-0.15, BORE - 0.036), (-0.07, BORE - 0.04), (-0.055, BORE - 0.05), (-0.01, BORE - 0.05), (-0.006, BORE - 0.06),
           (0.0, -0.035), (0.018, -0.064), (0.052, -0.07), (0.058, -0.06), (0.05, -0.02), (0.1, -0.03), (0.2, -0.05), (0.34, -0.073), (0.352, -0.07), (0.352, 0.064),
           (0.31, 0.068), (0.16, 0.064), (0.09, 0.056), (0.07, BORE + 0.003), (0.05, BORE - 0.002), (RX1 + 0.004, BORE - 0.006), (RX0 - 0.01, BORE - 0.006),
           (-0.36, BORE - 0.002)]
    s = L.side_prism(pts, -0.0205, 0.0205, bevel=0.006, segs=4, mat="wood_walnut", angle=15)
    # Inletting channel for the barrel / action.
    s.cut(L.cyl((0, BORE + 0.002, -0.41), (0, BORE + 0.002, RX1 + 0.01), 0.0158, 36, bevel=0))
    s.cut(L.box((0, BORE - 0.022, -0.04), (0.0095, 0.012, 0.06), 0, 0))          # magazine well (the floorplate covers it)
    s.cut(L.box((0, -0.004, -0.028), (0.0075, 0.016, 0.022), 0, 0))              # trigger well
    # Checkering (grip and forend), raised cheek piece on the left.
    grip_panel = lambda y, z: -0.058 < y < -0.008 and (0.006 + (-y) * 0.15) < z < (0.042 + (-y) * 0.3)
    fore_panel = lambda y, z: BORE - 0.033 < y < BORE - 0.012 and -0.33 < z < -0.18
    for sx in (1, -1):
        s.cut(L.stipple(sx * 0.0205, -0.06, -0.006, 0.0, 0.06, pitch=0.0028, r=0.0008, depth=0.0005, side=sx, inside=grip_panel, mat="wood_walnut"))
        s.cut(L.stipple(sx * 0.0205, BORE - 0.035, BORE - 0.01, -0.335, -0.175, pitch=0.0028, r=0.0008, depth=0.0005, side=sx, inside=fore_panel, mat="wood_walnut"))
    cheek = L.side_prism([(0.15, 0.064), (0.31, 0.066), (0.31, 0.022), (0.16, 0.03)], -0.025, -0.012, bevel=0.005, segs=4, mat="wood_walnut", angle=15)
    pad = L.side_prism([(0.352, 0.065), (0.372, 0.064), (0.373, -0.07), (0.352, -0.072)], -0.021, 0.021, bevel=0.004, segs=3, mat="rubber", angle=15)
    spacer = L.side_prism([(0.3505, 0.065), (0.3535, 0.065), (0.3535, -0.072), (0.3505, -0.072)], -0.0212, 0.0212, bevel=0.0008, segs=1, mat="plastic_white")
    cap = L.side_prism([(0.052, -0.07), (0.058, -0.06), (0.06, -0.064), (0.054, -0.074)], -0.012, 0.012, bevel=0.002, segs=2, mat="plastic_black")
    studs = [L.lathe([(0, 0), (0.0045, 0), (0.0045, 0.004), (0.003, 0.005), (0, 0.005)], (0, BORE - 0.035, -0.37), (0, -1, 0), 16, "steel_blued"),
             L.lathe([(0, 0), (0.0045, 0), (0.0045, 0.004), (0.003, 0.005), (0, 0.005)], (0, -0.067, 0.3), (0, -1, 0.2), 16, "steel_blued")]
    return [s, cheek, pad, spacer, cap] + studs


def bottom_metal():
    tg = L.side_prism([(-0.05, BORE - 0.05), (0.022, BORE - 0.05), (0.022, BORE - 0.054), (0.01, BORE - 0.054), (0.004, -0.024), (-0.012, -0.028), (-0.03, -0.024),
                       (-0.038, BORE - 0.054), (-0.05, BORE - 0.054)], -0.006, 0.006, bevel=0.0012, segs=2, mat="steel_blued", angle=20)
    tg.cut(L.side_prism([(-0.031, BORE - 0.054), (0.006, BORE - 0.054), (-0.001, -0.02), (-0.012, -0.023), (-0.025, -0.02)], -0.01, 0.01, 0, 0))
    floor = L.box((0, BORE - 0.0515, -0.075), (0.0098, 0.0015, 0.035), 0.0006, 1, "steel_blued")
    release = L.box((0, BORE - 0.0515, -0.034), (0.003, 0.0012, 0.003), 0.0005, 1, "steel_blued")
    screws = [L.screw((0, BORE - 0.0535, z), (0, -1, 0), 0.0028, 0.0006) for z in (-0.105, 0.017)]
    return [tg, floor, release] + screws


def build(ctx):
    it = L.Item("sniper", "weapon")
    it.part("receiver").add(*receiver())
    it.part("bolt", pivot=(0, BORE, 0.0), mech="bolt").add(*bolt())
    it.part("barrel").add(*barrel())
    fs, hood = front_sight()
    it.part("front_sight_base").add(*fs)
    it.part("front_sight", hide="optic").add(*hood)
    rb, rleaf, rh = P.flip_rear_sight(0.0, RAIL_TOP, LINE, "steel_blued")
    rs = it.part("rear_sight", hide="optic").add(*rb)
    it.part("rear_sight_leaf", parent=rs, pivot=tuple(rh)).add(*rleaf)
    it.part("stock", hide="stock").add(*stock())
    it.part("bottom_metal").add(*bottom_metal())
    it.part("trigger", pivot=(0, 0.004, -0.02), mech="trigger").add(P.ar_trigger((0, 0.004, -0.02), "steel_bare"))
    sf = L.side_prism([(0.034, BORE - 0.006), (0.048, BORE - 0.006), (0.048, BORE - 0.002), (0.034, BORE - 0.002)], 0.0145, 0.0175, bevel=0.0006, segs=1, mat="steel_blued")
    sf.cut(L.serrations(0.04, 0.048, BORE - 0.006, BORE - 0.002, 0.0175, 0.0012, 0.0003))
    it.part("safety", pivot=(0.016, BORE - 0.004, 0.04), mech="safety").add(sf)
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (0, 0.02, -0.36))
    it.socket("sight", (0, LINE, 0.0))
    it.socket("muzzle", (0, BORE, MUZZLE))
    it.socket("optic", (0, RAIL_TOP, -0.04))
    it.socket("stock", (0, 0.02, 0.08))
    it.socket("ejection_port", (0.0148, BORE + 0.004, -0.045), (0, 90, 0))
    it.socket("shell_eject", (0.02, BORE + 0.006, -0.04), (0, 70, 0))
    it.socket("bolt_handle", (0.045, BORE + 0.0046, 0.043))
    return it
