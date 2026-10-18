"""Shotgun (data/weapons/03_shotgun.json): 12-gauge pump action (870 pattern): blued steel receiver with a
top rail, ejection / loading ports, cross-bolt safety, slide release; barrel with a ventilated rib and a
ghost-ring front post; magazine tube with an end cap and sling swivel; ribbed walnut pump (forend) on
twin action bars; walnut stock with a pistol-grip wrist and a recoil pad. Procedural (path b).

Fixed by the JSON: bore y 0.055, muzzle z -0.64, rail top 0.077 (optic at z 0.02), sight line 0.087
(rear notch z -0.06, front post z -0.6), support hand (0, 0, -0.32) on the pump, stock (0, 0.01, 0.03)."""
import _parts as P
import item_lib as L

BORE = 0.055
RAIL_TOP = 0.077
RAIL_BASE = RAIL_TOP - 0.0094
LINE = 0.087
MUZZLE = -0.64
MAG_Y = 0.024
RZ0, RZ1 = -0.175, 0.03


def receiver():
    hw = 0.0152
    # The receiver's rear top slopes down to the stock (a flat top to the back crosses the near plane in ADS).
    pts = [(RZ0, 0.006), (RZ1 - 0.02, 0.006), (RZ1, 0.016), (RZ1, RAIL_BASE - 0.02), (RZ1 - 0.03, RAIL_BASE), (RZ0, RAIL_BASE)]
    r = L.side_prism(pts, -hw, hw, bevel=0.0018, segs=3, mat="steel_blued")
    r.cut(L.box((hw, BORE + 0.002, -0.09), (0.005, 0.011, 0.036), 0, 0))            # ejection port (right)
    r.cut(L.box((0, 0.006, -0.11), (0.0105, 0.006, 0.05), 0, 0))                    # loading port (bottom)
    r.cut(L.cyl((0, BORE, RZ0 - 0.01), (0, BORE, -0.03), 0.0118, 32, bevel=0))
    # The rail runs to z 0.025 (the optic slot's mounts sit on it), carried over the sloped rear by a riser;
    # 4.5 cm ahead of the ADS eye it stays clear of the near plane.
    rail = L.picatinny(-0.1, 0.025, RAIL_BASE, mat="aluminium_anodized")
    riser = L.side_prism([(-0.012, RAIL_BASE), (0.025, RAIL_BASE), (0.025, RAIL_BASE - 0.006), (0.004, RAIL_BASE - 0.012)], -0.0098, 0.0098, bevel=0.0008, segs=2,
                         mat="aluminium_anodized")
    rail.union(riser)
    elev = L.box((0, 0.006 + 0.0015, -0.11), (0.0098, 0.0015, 0.045), 0.0004, 1, "steel_dark")   # shell lifter (elevator) in the port
    marks = [L.text("DEADGROUND  870  12 GA", (-hw, 0.045, -0.085), 0.0042, (-1, 0, 0), (0, 1, 0), 0.0002),
             L.text("3\" CHAMBER", (-hw, 0.038, -0.085), 0.0028, (-1, 0, 0), (0, 1, 0), 0.00015)]
    pins = [L.pin((0, 0.014, z), (1, 0, 0), 0.0018, 2 * hw + 0.0008, "steel_dark") for z in (-0.02, 0.012)]
    return [r, rail, elev] + marks + pins


def rear_sight():
    # Ghost-ring-free notch blade on a base clamped to the rail.
    base = L.box((0, RAIL_TOP + 0.0018, -0.06), (0.0108, 0.0018, 0.008), 0.0006, 2, "steel_dark")
    blade = L.front_prism([(-0.009, RAIL_TOP + 0.0034), (0.009, RAIL_TOP + 0.0034), (0.009, LINE), (0.00275, LINE), (0.00275, LINE - 0.0035), (-0.00275, LINE - 0.0035),
                           (-0.00275, LINE), (-0.009, LINE)], -0.0635, -0.0565, bevel=0.0005, segs=2, mat="steel_dark")
    return [base, blade]


def barrel():
    Lb = RZ0 - MUZZLE
    prof = [(0, 0), (0.0142, 0), (0.0142, 0.03), (0.0124, 0.036), (0.0118, Lb - 0.006), (0.012, Lb - 0.002), (0.0, Lb)]
    b = L.lathe(prof, (0, BORE, RZ0), (0, 0, -1), 48, "steel_blued")
    b.cut(L.cyl((0, BORE, MUZZLE - 0.003), (0, BORE, MUZZLE + 0.02), 0.0093, 32, bevel=0))
    # Ventilated rib: a flat strip on posts along the top.
    rib = L.box((0, BORE + 0.016, (RZ0 + MUZZLE) * 0.5 - 0.005), (0.004, 0.0012, Lb * 0.5 - 0.012), 0.0004, 1, "steel_blued")
    posts = L.array(L.box((0, BORE + 0.0135, RZ0 - 0.03), (0.0026, 0.0018, 0.0026), 0.0004, 1, "steel_blued"), 12, (0, 0, -(Lb - 0.06) / 11.0))
    # Barrel ring that clamps the magazine tube cap.
    ring = L.box((0, (BORE + MAG_Y) * 0.5 - 0.002, -0.585), (0.009, 0.022, 0.009), 0.002, 2, "steel_blued")
    ring.cut(L.cyl((0, MAG_Y, -0.6), (0, MAG_Y, -0.57), 0.0115, 24, bevel=0))
    ring.cut(L.cyl((0, BORE, -0.6), (0, BORE, -0.57), 0.0119, 24, bevel=0))
    return [b, rib, posts, ring]


def front_sight():
    z = -0.6
    base = L.side_prism([(z - 0.008, BORE + 0.012), (z + 0.012, BORE + 0.012), (z + 0.004, BORE + 0.024), (z - 0.006, BORE + 0.024)], -0.0042, 0.0042, bevel=0.0008, segs=2,
                        mat="steel_blued")
    post = L.front_prism([(-0.002, BORE + 0.023), (0.002, BORE + 0.023), (0.002, LINE), (-0.002, LINE)], z - 0.002, z + 0.002, 0.0004, 1, "steel_dark")
    dot = L.cyl((0, LINE - 0.0028, z + 0.0021), (0, LINE - 0.0028, z + 0.0024), 0.0013, 12, "paint_orange", bevel=0)
    return [base, post, dot]


def mag_tube():
    t = L.cyl((0, MAG_Y, RZ0 + 0.002), (0, MAG_Y, -0.592), 0.0105, 36, "steel_blued", bevel=0.0006)
    cap = L.lathe([(0, 0), (0.0122, 0), (0.0125, 0.003), (0.0125, 0.018), (0.0108, 0.024), (0.004, 0.026), (0, 0.026)], (0, MAG_Y, -0.592), (0, 0, -1), 36,
                  "steel_blued")
    cap.cut(L.knurl((0, MAG_Y, -0.596), (0, MAG_Y, -0.608), 0.0125, 24, 0.0005))
    swivel = L.torus((0, MAG_Y - 0.004, -0.62), (1, 0, 0), 0.0075, 0.0014, 24, 8, "steel_blued")
    return [t, cap, swivel]


def pump():
    # Ribbed walnut forend around the magazine tube, action bars back into the receiver.
    z0, z1 = -0.18, -0.42
    pts = [(-0.011, MAG_Y - 0.023), (0.011, MAG_Y - 0.023), (0.022, MAG_Y - 0.012), (0.024, MAG_Y + 0.006), (0.02, MAG_Y + 0.02), (0.012, MAG_Y + 0.024), (-0.012, MAG_Y + 0.024),
           (-0.02, MAG_Y + 0.02), (-0.024, MAG_Y + 0.006), (-0.022, MAG_Y - 0.012)]
    f = L.front_prism(pts, z1, z0, bevel=0.004, segs=3, mat="wood_walnut", angle=15)
    f.cut(L.cyl((0, MAG_Y, z1 - 0.01), (0, MAG_Y, z0 + 0.01), 0.0112, 32, bevel=0))
    grooves = []
    for k in range(14):
        z = z1 + 0.035 + k * 0.012
        grooves.append(L.torus((0, MAG_Y, z), (0, 0, 1), 0.0245, 0.0016, 36, 6, "wood_walnut"))
    f.cut(L.merge(*grooves))
    bars = [L.box((sx * 0.0112, MAG_Y + 0.006, (z0 + RZ0 + 0.08) * 0.5), (0.0009, 0.0028, (z0 - RZ0 - 0.08) * -0.5 + 0.01), 0.0003, 1, "steel_dark") for sx in (1, -1)]
    collar = L.ring_lathe([(0.0112, 0), (0.0135, 0), (0.0135, 0.006), (0.0112, 0.006)], (0, MAG_Y, z0 + 0.006), (0, 0, -1), 32, "steel_blued")
    return [f, collar] + bars


def trigger_group():
    hw = 0.0115
    tg = L.side_prism([(-0.075, 0.007), (0.02, 0.007), (0.02, -0.002), (0.004, -0.008), (-0.0, -0.022), (-0.03, -0.027), (-0.055, -0.022), (-0.06, -0.005), (-0.075, -0.004)],
                      -hw, hw, bevel=0.0015, segs=2, mat="aluminium_anodized", angle=20)
    tg.cut(L.side_prism([(-0.053, -0.004), (-0.004, -0.004), (-0.006, -0.017), (-0.03, -0.022), (-0.048, -0.017)], -0.02, 0.02, 0, 0))
    safety = L.cyl((-hw - 0.0018, -0.0, -0.0), (hw + 0.0018, -0.0, -0.0), 0.0032, 20, "steel_dark", bevel=0.0005)
    ring = L.cyl((hw + 0.0018, 0, 0), (hw + 0.0022, 0, 0), 0.0016, 12, "paint_red", bevel=0)
    release = L.side_prism([(-0.078, 0.0), (-0.06, -0.0), (-0.058, -0.004), (-0.074, -0.004)], -hw - 0.001, -hw + 0.001, 0.0005, 1, "steel_dark")
    return [tg, release], [safety, ring]


def stock():
    y = 0.01
    # Comb drop ~40 mm below the sight line where the cheek rests (a higher comb crosses the camera's near
    # plane in ADS: tools/tests/item_nearplane.cfg).
    pts = [(RZ1, 0.058), (0.06, 0.046), (0.25, 0.054), (0.305, 0.062), (0.305, -0.075), (0.25, -0.07), (0.1, -0.045), (0.05, -0.065), (0.024, -0.068), (0.018, -0.03),
           (RZ1, 0.006)]
    s = L.side_prism(pts, -0.0205, 0.0205, bevel=0.006, segs=4, mat="wood_walnut", angle=15)
    panel = lambda yy, z: -0.06 < yy < -0.006 and (0.02 + (-yy) * 0.1) < z < (0.06 + (-yy) * 0.25)
    for sx in (1, -1):
        s.cut(L.stipple(sx * 0.0205, -0.062, -0.004, 0.015, 0.08, pitch=0.0028, r=0.0008, depth=0.0005, side=sx, inside=panel, mat="wood_walnut"))
    pad = L.side_prism([(0.305, 0.062), (0.326, 0.061), (0.327, -0.074), (0.305, -0.076)], -0.0212, 0.0212, bevel=0.004, segs=3, mat="rubber", angle=15)
    pad.cut(L.merge(*[L.box((0, 0.058 - k * 0.0095, 0.3265), (0.023, 0.0013, 0.0013), 0, 0) for k in range(14)]))
    cap = L.side_prism([(0.024, -0.068), (0.05, -0.065), (0.052, -0.07), (0.026, -0.073)], -0.012, 0.012, bevel=0.002, segs=2, mat="plastic_black")
    stud = L.lathe([(0, 0), (0.0045, 0), (0.0045, 0.004), (0.003, 0.005), (0, 0.005)], (0, -0.072, 0.27), (0, -1, 0.15), 16, "steel_blued")
    return [s, pad, cap, stud]


def build(ctx):
    it = L.Item("shotgun", "weapon")
    it.part("receiver").add(*receiver())
    it.part("rear_sight", hide="optic").add(*rear_sight())
    it.part("barrel").add(*barrel())
    it.part("front_sight", hide="optic").add(*front_sight())
    it.part("mag_tube").add(*mag_tube())
    it.part("pump", pivot=(0, MAG_Y, -0.3), mech="pump").add(*pump())
    bolt = L.box((0.0, BORE, -0.075), (0.0098, 0.0098, 0.03), 0.0012, 2, "steel_bare")
    bolt.cut(L.box((0.0098, BORE + 0.004, -0.088), (0.002, 0.004, 0.01), 0, 0))
    it.part("bolt", pivot=(0, BORE, -0.075), mech="pump").add(bolt)
    tg, sf = trigger_group()
    it.part("trigger_group").add(*tg)
    it.part("safety", pivot=(0, 0, 0), mech="safety").add(*sf)
    it.part("trigger", pivot=(0, 0.003, -0.032), mech="trigger").add(P.ar_trigger((0, 0.003, -0.032), "steel_dark"))
    it.part("stock", hide="stock").add(*stock())
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (0, 0.0, -0.32))
    it.socket("sight", (0, LINE, -0.06))
    it.socket("muzzle", (0, BORE, MUZZLE))
    it.socket("optic", (0, RAIL_TOP, 0.02))
    it.socket("stock", (0, 0.01, 0.03))
    it.socket("ejection_port", (0.0152, BORE + 0.002, -0.09), (0, 90, 0))
    it.socket("shell_eject", (0.02, BORE + 0.004, -0.085), (0, 75, 0))
    it.socket("loading_port", (0, 0.006, -0.11), (90, 0, 0))
    return it
