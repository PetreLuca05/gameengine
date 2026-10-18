"""Shooting glasses (data/items.json "glasses"): wraparound ballistic eyewear: two tinted polycarbonate
lenses (curved, the nose cut-out), the black frame brow bar and rubber nose pads, both temple arms
folded behind the lenses on their hinges (separate parts). Real size 14 x 5 x 6 cm folded, palm-held.
Procedural (path b)."""
import item_lib as L


def lens(sx):
    """One lens: an outline extruded 2 mm, bent round the face (z follows x squared)."""
    pts = [(sx * 0.006, 0.0), (sx * 0.012, -0.014), (sx * 0.03, -0.02), (sx * 0.058, -0.012), (sx * 0.066, 0.006), (sx * 0.062, 0.018), (sx * 0.032, 0.02), (sx * 0.008, 0.016)]
    if sx < 0:
        pts = list(reversed(pts))
    s = L.front_prism(pts, -0.001, 0.001, bevel=0.0004, segs=2, mat="glass_amber")
    for vt in s.bm.verts:
        vt.co.z += (vt.co.x / 0.066) ** 2 * 0.022
    return s


def build(ctx):
    it = L.Item("glasses", "item")
    lenses = [lens(1), lens(-1)]
    brow = L.front_prism([(-0.068, 0.017), (0.068, 0.017), (0.068, 0.023), (0.04, 0.026), (-0.04, 0.026), (-0.068, 0.023)], -0.0025, 0.0025, bevel=0.0006, segs=2,
                         mat="polymer_matte")
    for vt in brow.bm.verts:
        vt.co.z += (vt.co.x / 0.066) ** 2 * 0.022
    bridge = L.box((0, 0.008, 0.0), (0.006, 0.01, 0.003), 0.002, 2, "polymer_matte")
    pads = [L.box((sx * 0.007, -0.004, 0.006), (0.0025, 0.006, 0.0035), 0.0015, 2, "rubber") for sx in (1, -1)]
    it.part("frame").add(*lenses, brow, bridge, *pads)
    for sx in (1, -1):
        hinge = (sx * 0.068, 0.019, 0.022)
        # Folded: the arm runs back across behind the lenses from its hinge, the left one a little lower.
        y = 0.016 if sx > 0 else 0.009
        z = 0.034 if sx > 0 else 0.038
        arm = L.box((sx * 0.0115, y, z), (0.0565, 0.003, 0.0015), 0.0008, 2, "polymer_matte")
        knuckle = L.box((sx * 0.066, (y + 0.019) * 0.5, (z + 0.022) * 0.5), (0.003, 0.004, (z - 0.022) * 0.5 + 0.002), 0.001, 2, "polymer_matte")
        tip = L.box((sx * -0.036, y - 0.001, z), (0.012, 0.0045, 0.0025), 0.002, 2, "rubber")
        it.part("temple_r" if sx > 0 else "temple_l", pivot=hinge, mech="fold").add(arm, knuckle, tip)
    it.socket("grip_r", (0, 0, 0))
    it.socket("bridge", (0, 0.008, 0.0))
    it.hold_small()
    return it
