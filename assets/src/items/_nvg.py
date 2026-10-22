"""Night vision goggles builder (docs/NIGHT_OPTICS.md): a Gen-3 intensifier tube monocular (PVS-14 style)
or a binocular pair on a bridge: objective lens with its focus ring, the tube housing with the battery
compartment (one AA) and its cap, the power / IR switch, the IR illuminator window beside the objective,
the eyepiece with its rubber eyecup and diopter ring, and the J-arm / dovetail shoe that mounts it on a
helmet (the flip pivot: socket "flip_pivot"). Axis along -Z (the lens looks forward). Palm-held."""
import item_lib as L


def _tube(x, glass, housing="polymer_od"):
    out = []
    # housing: objective end at z = -0.075, eyepiece end at z = +0.035 (along +Z)
    body = L.lathe([(0, 0), (0.0205, 0), (0.0215, 0.003), (0.0215, 0.07), (0.0195, 0.074), (0.0195, 0.082), (0, 0.082)], (x, 0, -0.06), (0, 0, 1), 48, housing)
    out.append(body)
    obj = L.lathe([(0.0185, 0), (0.0235, 0.0), (0.0245, 0.003), (0.0245, 0.026), (0.0235, 0.029), (0.0185, 0.029)], (x, 0, -0.088), (0, 0, 1), 48, "rubber")
    out.append(obj)
    out.append(L.knurl((x, 0, -0.084), (x, 0, -0.064), 0.0247, 36, 0.0004, "rubber"))
    out.append(L.cyl((x, 0, -0.0855), (x, 0, -0.0835), 0.0182, 40, glass, bevel=0))   # objective lens (coated)
    eye = L.lathe([(0.0125, 0), (0.0175, 0), (0.0175, 0.012), (0.0185, 0.016), (0.0185, 0.028), (0.016, 0.03), (0.0125, 0.03)], (x, 0, 0.022), (0, 0, 1), 40, "rubber")
    out.append(eye)
    out.append(L.cyl((x, 0, 0.037), (x, 0, 0.039), 0.0124, 32, "glass_green", bevel=0))   # eyepiece (phosphor glow)
    out.append(L.ring_lathe([(0.0196, 0), (0.0212, 0), (0.0212, 0.007), (0.0196, 0.007)], (x, 0, 0.012), (0, 0, 1), 40, "polymer_matte"))   # diopter ring
    return out


def nvg(item_id, dual=False, white=False, label="NVG"):
    it = L.Item(item_id, "item")
    glass = "glass_lens"
    xs = (-0.032, 0.032) if dual else (0.0,)
    tubes = []
    for x in xs:
        tubes += _tube(x, glass)
    it.part("tubes").add(*tubes)
    parts = []
    if dual:
        bridge = L.box((0, 0.012, -0.02), (0.032, 0.009, 0.02), 0.003, 2, "polymer_od")
        parts.append(bridge)
        parts.append(L.cyl((0, 0.022, -0.035), (0, 0.022, -0.005), 0.007, 24, "polymer_matte"))   # IPD hinge
    # battery compartment (one AA) on the side, its cap; the power / IR knob on top
    bx = (xs[-1] + 0.0215) if not dual else 0.0
    if dual:
        parts.append(L.cyl((-0.015, 0.03, -0.05), (0.015, 0.03, -0.05), 0.0085, 32, "polymer_od"))
        parts.append(L.cyl((0.015, 0.03, -0.05), (0.021, 0.03, -0.05), 0.0092, 32, "polymer_matte"))
    else:
        parts.append(L.cyl((bx - 0.004, 0.0, -0.045), (bx + 0.03, 0.0, -0.045), 0.0085, 32, "polymer_od"))
        parts.append(L.cyl((bx + 0.03, 0.0, -0.045), (bx + 0.036, 0.0, -0.045), 0.0092, 32, "polymer_matte"))
    knob = L.cyl((0, 0.021, -0.01), (0, 0.029, -0.01), 0.0065, 20, "polymer_matte")
    parts.append(knob)
    parts.append(L.text("OFF ON IR", (0, 0.0297, -0.01), 0.0022, (0, 1, 0), (0, 0, -1), 0.0001, "paint_white"))
    # IR illuminator window beside the objective
    ir_x = (xs[0] - 0.03) if dual else -0.03
    parts.append(L.box((ir_x, -0.004, -0.07), (0.007, 0.007, 0.012), 0.002, 2, "polymer_od"))
    parts.append(L.cyl((ir_x, -0.004, -0.0835), (ir_x, -0.004, -0.0815), 0.0045, 20, "glass_amber", bevel=0))
    parts.append(L.text(label, (xs[0] - 0.0216, 0, -0.03), 0.004, (-1, 0, 0), (0, 1, 0), 0.0001, "paint_white"))
    it.part("body").add(*parts)
    # J-arm and the dovetail shoe (the helmet mount); the flip pivot at its top
    arm = [L.box((0, 0.036, -0.01), (0.008, 0.012, 0.006), 0.002, 2, "aluminium_anodized"),
           L.box((0, 0.052, -0.004), (0.014, 0.005, 0.012), 0.0015, 2, "aluminium_anodized"),
           L.cyl((-0.016, 0.05, 0.004), (0.016, 0.05, 0.004), 0.0035, 20, "steel_blued")]
    it.part("mount", pivot=(0, 0.05, 0.004), mech="flip").add(*arm)
    it.socket("flip_pivot", (0, 0.05, 0.004))
    it.socket("grip_r", (0, 0, 0))
    it.socket("ir_emitter", (ir_x, -0.004, -0.084))
    it.hold_small()
    return it
