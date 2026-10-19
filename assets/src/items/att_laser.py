"""Laser (assets/attachments/laser.json, new): aiming laser / IR illuminator module under the rail:
squared aluminium housing, two apertures (visible red laser + IR), windage / elevation adjusters with
covers, battery cap, a pressure-pad port and an activation button. The flashlight toggle (F) turns the
beam on: a red line from the emitter and a dot where it lands. Procedural (path b)."""
import _light as G
import _optics as O
import item_lib as L


def build(ctx):
    it = L.Item("att_laser", "attachment")
    body = L.box((0, -0.017, -0.012), (0.0165, 0.0105, 0.033), 0.0028, 3, "aluminium_fde")
    body.cut(L.cyl((-0.007, -0.016, -0.05), (-0.007, -0.016, -0.04), 0.0046, 24, bevel=0))
    body.cut(L.cyl((0.0065, -0.019, -0.05), (0.0065, -0.019, -0.04), 0.0038, 24, bevel=0))
    lens_v = O.lens((-0.007, -0.016, -0.0435), (0, 0, -1), 0.0045, "glass_lens", 0.0004)
    lens_ir = O.lens((0.0065, -0.019, -0.0435), (0, 0, -1), 0.0037, "glass_amber", 0.0004)
    rim = [L.ring_lathe([(0.0046, 0), (0.0058, 0), (0.0058, 0.0025), (0.0046, 0.0025)], c, (0, 0, -1), 32, "aluminium_fde") for c in ((-0.007, -0.016, -0.0445),)]
    adj = O.turret((0.0165, -0.017, -0.005), (1, 0, 0), 0.0042, 0.004, marks=6) + O.turret((0, -0.0275, -0.005), (0, -1, 0), 0.0042, 0.004, marks=6)
    cap = L.cyl((-0.0165, -0.017, 0.006), (-0.024, -0.017, 0.006), 0.0068, 32, "aluminium_fde", bevel=0.0008)
    cap.cut(L.knurl((-0.0168, -0.017, 0.006), (-0.024, -0.017, 0.006), 0.0068, 24, 0.0004))
    button = L.box((0, -0.0145, 0.022), (0.006, 0.004, 0.004), 0.0015, 2, "rubber")
    port = L.cyl((0.0165, -0.017, 0.014), (0.019, -0.017, 0.014), 0.0028, 16, "rubber", bevel=0.0004)
    marks = [L.text("DANGER  LASER  CLASS 3R", (-0.0166, -0.012, -0.02), 0.0019, (-1, 0, 0), (0, 1, 0), 0.0001, "paint_yellow"),
             L.text("DG-L2", (-0.0166, -0.022, -0.02), 0.0032, (-1, 0, 0), (0, 1, 0), 0.0001, "paint_black")]
    it.part("clamp").add(*G.hanging_clamp(-0.02, 0.016, "aluminium_fde"))
    it.part("body").add(body, lens_v, lens_ir, *rim, *adj, cap, port, *marks)
    it.part("button", mech="switch").add(button)
    it.socket("mount", (0, 0, 0))
    it.socket("emitter", (-0.007, -0.016, -0.046))
    it.socket("ir_emitter", (0.0065, -0.019, -0.046))
    return it
