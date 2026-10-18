"""Fuel can (data/items.json "fuel_can"): a 10 l steel jerry can: pressed body with the X stiffening
beads, the triple handle on top (the hand on the middle bar = grip_r at the origin), the bayonet spout
cap with its cam lever (separate, hinged) and an air-breather tube, the flexible pour spout clipped to the
side (separate), red paint with stencilled markings. Procedural (path b)."""
import item_lib as L

W, H, D = 0.24, 0.32, 0.14


def build(ctx):
    it = L.Item("fuel_can", "item")
    top = -0.022   # the can's top below the hand
    body = L.box((0, top - H * 0.5, 0), (W * 0.5, H * 0.5, D * 0.5), 0.012, 4, "paint_red")
    seam = L.box((0, top - H * 0.5, 0), (W * 0.5 + 0.0018, H * 0.5 + 0.0018, 0.0022), 0.002, 2, "paint_red")
    # X beads pressed into both faces (raised ridges).
    beads = []
    for sz in (1, -1):
        for sgn in (1, -1):
            b = L.box((0, top - H * 0.5, sz * (D * 0.5 + 0.0006)), (0.19 * 0.5 * 1.15, 0.006, 0.0018), 0.0016, 3, "paint_red")
            b.rotate((0, 0, 1), sgn * 52, (0, top - H * 0.5, 0))
            beads.append(b)
        beads.append(L.box((0, top - H * 0.5, sz * (D * 0.5 + 0.0004)), (W * 0.5 - 0.02, H * 0.5 - 0.02, 0.0004), 0.003, 2, "paint_red"))
    # Triple handle bars on struts.
    handles = []
    for x in (-0.06, 0.0, 0.06):
        handles.append(L.cyl((x, 0.0, -0.035), (x, 0.0, 0.035), 0.0085, 24, "paint_red", bevel=0.001))
        handles.append(L.box((x, top * 0.5, -0.04), (0.007, -top * 0.5 + 0.004, 0.006), 0.0018, 2, "paint_red"))
        handles.append(L.box((x, top * 0.5, 0.04), (0.007, -top * 0.5 + 0.004, 0.006), 0.0018, 2, "paint_red"))
    marks = [L.text("FUEL  10 L", (0, top - 0.06, D * 0.5 + 0.0028), 0.022, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow"),
             L.text("DG-GAS  UN1203", (0, top - 0.26, D * 0.5 + 0.0028), 0.009, (0, 0, 1), (0, 1, 0), 0.0002, "paint_yellow")]
    neck = L.cyl((W * 0.5 - 0.04, top, 0.0), (W * 0.5 - 0.04, top + 0.014, 0.0), 0.021, 40, "steel_dark", bevel=0.001)
    cap = L.lathe([(0, 0), (0.025, 0), (0.025, 0.012), (0.022, 0.016), (0, 0.016)], (W * 0.5 - 0.04, top + 0.014, 0.0), (0, 1, 0), 48, "paint_red")
    lever = L.side_prism([(-0.02, top + 0.03), (0.02, top + 0.03), (0.024, top + 0.022), (-0.012, top + 0.022)], -0.007, 0.007, bevel=0.0015, segs=2, mat="steel_dark")
    lever.rotate((0, 1, 0), 90)
    lever.move((W * 0.5 - 0.04, 0, 0))
    pin = L.cyl((W * 0.5 - 0.04, top + 0.022, -0.012), (W * 0.5 - 0.04, top + 0.022, 0.012), 0.0022, 12, "steel_bare", bevel=0.0003)
    breather = L.cyl((W * 0.5 - 0.075, top, 0.0), (W * 0.5 - 0.075, top + 0.01, 0.0), 0.006, 20, "steel_dark", bevel=0.0006)
    spout = L.lathe([(0.016, 0), (0.017, 0.004), (0.011, 0.014), (0.0105, 0.16), (0.0085, 0.17), (0.007, 0.175), (0.0055, 0.175), (0.0055, 0.005), (0.014, 0.004),
                     (0.014, 0)], (-W * 0.5 - 0.016, top - 0.28, 0.0), (0, 1, 0), 32, "plastic_yellow")
    spout.cut(L.merge(*[L.torus((-W * 0.5 - 0.016, top - 0.26 + k * 0.012, 0), (0, 1, 0), 0.0108, 0.0012, 32, 6) for k in range(9)]))
    strap = [L.box((-W * 0.5 - 0.009, top - y, 0.0), (0.009, 0.006, 0.018), 0.0015, 2, "webbing_black") for y in (0.06, 0.24)]
    it.part("body").add(body, seam, *beads, *handles, *marks, neck, breather, *strap)
    it.part("cap", pivot=(W * 0.5 - 0.04, top + 0.022, 0.0), mech="cap").add(cap, lever, pin)
    it.part("spout", mech="spout").add(spout)
    it.socket("grip_r", (0, 0, 0))
    it.socket("spout", (W * 0.5 - 0.04, top + 0.03, 0.0))
    it.socket("pour", (-W * 0.5 - 0.016, top - 0.105, 0.0))
    return it
