"""Pistol (data/weapons/01_pistol.json): striker-fired 9 mm, polymer frame with an accessory rail, steel
slide with front / rear serrations and an optic cut, notch-and-post sights. Procedural (path b).

Fixed by the JSON: grip_r = origin, bore y 0.055, muzzle z -0.19, sight line 0.084 (rear notch z 0.032,
front post z -0.17), slide top 0.077 (optic plate at z -0.04), support hand (-0.035, -0.03, 0.01)."""
import _parts as P
import item_lib as L

BORE = 0.055
SLIDE_TOP = 0.077
LINE = 0.084
Z_FRONT, Z_REAR = -0.188, 0.036


def slide():
    hw = 0.0128
    # Side profile: flat top, chamfered top edges via the bevel, the muzzle end tapered (dehorned).
    pts = [(Z_FRONT, 0.047), (Z_FRONT, SLIDE_TOP - 0.004), (Z_FRONT + 0.006, SLIDE_TOP), (Z_REAR - 0.004, SLIDE_TOP), (Z_REAR, SLIDE_TOP - 0.006),
           (Z_REAR, 0.047)]
    s = L.side_prism(pts, -hw, hw, bevel=0.0018, segs=3, mat="steel_cerakote", angle=20)
    # Top edge chamfers: cut wedge prisms along both top corners.
    for sx in (1, -1):
        w = L.front_prism([(sx * hw, SLIDE_TOP - 0.006), (sx * hw, SLIDE_TOP + 0.001), (sx * (hw - 0.006), SLIDE_TOP + 0.001)], Z_FRONT - 0.01, Z_REAR + 0.01,
                          0, 0)
        s.cut(w)
    # Ejection port (right / top), rear and front cocking serrations, optic plate pocket, bore.
    s.cut(L.box((0.006, SLIDE_TOP - 0.004, -0.03), (0.0095, 0.009, 0.019), 0, 0))
    for sx in (1, -1):
        s.cut(L.serrations(0.004, 0.03, 0.05, SLIDE_TOP - 0.003, sx * hw, 0.0032, 0.0007))
        s.cut(L.serrations(-0.17, -0.142, 0.05, SLIDE_TOP - 0.004, sx * hw, 0.0032, 0.0007))
    s.cut(L.box((0, SLIDE_TOP, -0.04), (0.0112, 0.0014, 0.0255), 0, 0))
    s.cut(L.cyl((0, BORE, Z_FRONT - 0.002), (0, BORE, Z_FRONT + 0.012), 0.0068, 32, bevel=0))
    s.cut(L.cyl((0, 0.039 + 0.0045, Z_FRONT - 0.002), (0, 0.039 + 0.0045, Z_FRONT + 0.012), 0.0046, 24, bevel=0))
    # Optic plate (cover) with its two screws; extractor; firing pin channel cover plate at the back.
    plate = L.box((0, SLIDE_TOP - 0.0004, -0.04), (0.011, 0.0012, 0.025), 0.0005, 1, "steel_cerakote")
    screws = [L.screw((0, SLIDE_TOP + 0.0008, z), (0, 1, 0), 0.0019, 0.0005, slot=False, hexsock=True) for z in (-0.055, -0.025)]
    extr = L.box((hw + 0.0002, BORE + 0.006, -0.012), (0.0006, 0.0024, 0.011), 0.0003, 1, "steel_dark")
    cover = L.box((0, 0.059, Z_REAR + 0.0004), (0.0065, 0.008, 0.0006), 0.0004, 1, "polymer_matte")
    marks = [L.text("DG 19", (-hw, 0.064, -0.095), 0.0052, (-1, 0, 0), (0, 1, 0), 0.0002),
             L.text("9x19", (-hw, 0.0585, -0.095), 0.0036, (-1, 0, 0), (0, 1, 0), 0.00015),
             L.text("BKV417", (hw, 0.051, -0.07), 0.0028, (1, 0, 0), (0, 1, 0), 0.00015)]
    return [s, plate, extr, cover] + screws + marks


def sights():
    # Rear: dovetailed notch blade with white dots; front: post with a dot.
    rb = L.front_prism([(-0.0105, SLIDE_TOP - 0.0012), (0.0105, SLIDE_TOP - 0.0012), (0.0105, LINE - 0.0006), (0.0083, LINE), (0.00225, LINE),
                        (0.00225, LINE - 0.004), (-0.00225, LINE - 0.004), (-0.00225, LINE), (-0.0083, LINE), (-0.0105, LINE - 0.0006)], 0.026, 0.035,
                       bevel=0.0006, segs=2, mat="steel_dark")
    dots = [L.cyl((x, LINE - 0.002, 0.0255), (x, LINE - 0.002, 0.0258), 0.0011, 12, "paint_white", bevel=0) for x in (-0.0055, 0.0055)]
    fp = L.front_prism([(-0.0016, SLIDE_TOP - 0.001), (0.0016, SLIDE_TOP - 0.001), (0.0015, LINE), (-0.0015, LINE)], -0.1745, -0.1655, bevel=0.0004,
                       segs=1, mat="steel_dark")
    fbase = L.box((0, SLIDE_TOP + 0.0012, -0.17), (0.003, 0.0014, 0.0055), 0.0005, 1, "steel_dark")
    fdot = L.cyl((0, LINE - 0.0018, -0.1744), (0, LINE - 0.0018, -0.1748), 0.0011, 12, "paint_white", bevel=0)
    return [rb] + dots, [fp, fbase, fdot]


def frame():
    hw = 0.0118
    # Dust cover / rail section ahead of the trigger guard, the receiver under the slide.
    pts = [(-0.176, 0.047), (Z_REAR - 0.002, 0.047), (Z_REAR, 0.04), (0.03, 0.03), (0.004, 0.027), (-0.07, 0.028), (-0.075, 0.031), (-0.176, 0.031)]
    f = L.side_prism(pts, -hw, hw, bevel=0.0015, segs=2, mat="polymer_matte")
    rail = L.box((0, 0.0285, -0.138), (0.0105, 0.0028, 0.034), 0.0006, 1, "polymer_matte")
    rail.cut(L.array(L.box((0, 0.026, -0.158), (0.012, 0.0022, 0.0026), 0, 0), 3, (0, 0, 0.0181)))
    # Trigger guard: squared front with a hook.
    tg = L.side_prism([(-0.074, 0.031), (-0.07, 0.006), (-0.064, 0.0015), (-0.02, 0.0015), (-0.009, 0.006), (-0.006, 0.027), (-0.012, 0.027),
                       (-0.016, 0.009), (-0.022, 0.0058), (-0.062, 0.0058), (-0.0655, 0.009), (-0.068, 0.031)], -0.0052, 0.0052, bevel=0.0013,
                      segs=2, mat="polymer_matte", angle=20)
    # Grip: raked, with finger grooves on the front strap, beavertail, textured panels.
    g = L.side_prism([(-0.006, 0.027), (-0.012, 0.012), (-0.008, 0.004), (-0.0105, -0.012), (-0.006, -0.02), (-0.0085, -0.034), (-0.0035, -0.044),
                      (-0.004, -0.06), (0.003, -0.068), (0.034, -0.068), (0.034, -0.052), (0.028, -0.01), (0.03, 0.022), (0.042, 0.033), (0.04, 0.04),
                      (0.03, 0.03)][::-1], -0.0142, 0.0142, bevel=0.0035, segs=3, mat="polymer_matte", angle=18)
    # Stippled side panels (the texture the palm sees in first person), front and back straps grooved.
    panel = lambda y, z: -0.06 < y < 0.012 and (-0.004 + (0.012 - y) * 0.06) < z < (0.027 + (0.012 - y) * 0.03)
    for sx in (1, -1):
        g.cut(L.stipple(sx * 0.0142, -0.062, 0.012, -0.006, 0.03, side=sx, inside=panel))
    g.cut(L.merge(*[L.box((0, -0.002 - k * 0.006, 0.0315 - k * 0.0002), (0.016, 0.0009, 0.0012), 0, 0) for k in range(9)]))
    g.cut(L.box((0, -0.06, 0.016), (0.0105, 0.012, 0.015), 0, 0))   # mag well opening at the bottom
    mr = L.box((-0.0128, 0.016, -0.004), (0.0016, 0.0035, 0.004), 0.0008, 2, "polymer_matte")   # mag release (left)
    stop = L.side_prism([(-0.06, 0.044), (-0.03, 0.044), (-0.028, 0.048), (-0.058, 0.0485)], -hw - 0.0018, -hw, 0.0004, 1, "steel_dark")
    tdl = L.box((0, 0.042, -0.064), (hw + 0.0012, 0.0016, 0.0035), 0.0005, 1, "steel_dark")    # takedown lever
    pins = [L.pin((0, 0.036, z), (1, 0, 0), 0.0013, 2 * hw + 0.0006, "steel_dark") for z in (-0.0565, -0.0215, 0.014)]
    marks = [L.text("DEADGROUND  AUSTRIA", (-0.0143, -0.03, 0.014), 0.0028, (-1, 0, 0), (0, 1, 0), 0.00015, "polymer_matte")]
    return [f, rail, tg, g, mr, stop, tdl] + pins + marks


def barrel_and_guide():
    hood = L.box((0, BORE + 0.0045, -0.03), (0.0078, 0.006, 0.018), 0.0008, 2, "steel_dark")
    bar = L.cyl((0, BORE, -0.01), (0, BORE, Z_FRONT + 0.0015), 0.0068, 32, "steel_dark", bevel=0.0004)
    bar.cut(L.cyl((0, BORE, Z_FRONT - 0.002), (0, BORE, Z_FRONT + 0.01), 0.0046, 24, bevel=0))
    guide = L.cyl((0, 0.0435, -0.09), (0, 0.0435, Z_FRONT + 0.003), 0.0042, 20, "polymer_matte", bevel=0.0004)
    guide.cut(L.cyl((0, 0.0435, Z_FRONT), (0, 0.0435, Z_FRONT + 0.006), 0.0018, 12, bevel=0))
    return [hood, bar, guide]


def magazine():
    body = L.side_prism([(0.003, -0.05), (0.031, -0.05), (0.033, -0.069), (0.001, -0.069)], -0.0105, 0.0105, bevel=0.0012, segs=2, mat="polymer_matte")
    base = L.side_prism([(0.0, -0.068), (0.034, -0.068), (0.036, -0.073), (0.033, -0.0745), (0.001, -0.0745)], -0.0128, 0.0128, bevel=0.0016, segs=2,
                        mat="polymer_matte")
    return [body, base]


def build(ctx):
    it = L.Item("pistol", "weapon")
    sl = it.part("slide", mech="slide").add(*slide())
    rear, front = sights()
    it.part("rear_sight", parent=sl, hide="optic").add(*rear)
    it.part("front_sight", parent=sl, hide="optic").add(*front)
    it.part("frame").add(*frame())
    it.part("barrel").add(*barrel_and_guide())
    trig = P.ar_trigger((0, 0.025, -0.032), "polymer_matte")
    blade = L.box((0, 0.012, -0.0345), (0.0011, 0.007, 0.0012), 0.0003, 1, "polymer_matte")
    it.part("trigger", pivot=(0, 0.025, -0.032), mech="trigger").add(trig, blade)
    it.part("magazine").add(*magazine())
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (-0.035, -0.03, 0.01))
    it.socket("sight", (0, LINE, 0.032))
    it.socket("muzzle", (0, BORE, -0.19))
    it.socket("optic", (0, SLIDE_TOP, -0.04))
    it.socket("underbarrel", (0, 0.026, -0.138))
    it.socket("magazine", (0, -0.06, 0.017))
    it.socket("ejection_port", (0.006, SLIDE_TOP, -0.03), (-90, 0, 0))
    it.socket("shell_eject", (0.012, SLIDE_TOP + 0.004, -0.028), (-30, 80, 0))
    return it
