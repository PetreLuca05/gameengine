"""Red dot (assets/attachments/red_dot.json): micro tube reflex sight (T-2 pattern) on a low cantilever
Picatinny mount: 30 mm tube body with the open window (the sight pass draws the glass and the dot at the
JSON lens: (0, 0.027, -0.012), r 10.5 mm), flip-up front cap (open; the eye-side cap off, its ring kept), elevation / windage turrets with caps,
brightness dial and battery cap on the right, LED emitter housing under the window. Procedural (path b)."""
import _optics as O
import item_lib as L

AX_Y = 0.027
LENS_Z = -0.012


def body():
    r_in, r_out = 0.0112, 0.0142
    tube = L.ring_lathe([(r_in, 0.0), (r_out, 0.0), (r_out + 0.0012, 0.004), (r_out + 0.0012, 0.03), (r_out, 0.034), (r_in, 0.034)], (0, AX_Y, LENS_Z + 0.014), (0, 0, -1), 56,
                        "aluminium_anodized", bevel=0.0004)
    hoods = [L.ring_lathe([(r_in, 0.0), (r_out + 0.0005, 0.0), (r_out + 0.0005, 0.004), (r_in + 0.0006, 0.005)], (0, AX_Y, z), (0, 0, d), 56, "aluminium_anodized", bevel=0.0003)
             for (z, d) in ((LENS_Z - 0.0195, -1), (LENS_Z + 0.0135, 1))]
    saddle = L.box((0, AX_Y - 0.007, LENS_Z - 0.003), (0.0108, 0.007, 0.012), 0.0015, 2, "aluminium_anodized")
    saddle.cut(L.cyl((0, AX_Y, LENS_Z - 0.03), (0, AX_Y, LENS_Z + 0.03), r_in, 48, bevel=0))
    emitter = L.box((0, AX_Y - r_in + 0.001, LENS_Z + 0.006), (0.0022, 0.0014, 0.0022), 0.0004, 1, "plastic_black")
    led = L.cyl((0, AX_Y - r_in + 0.0024, LENS_Z + 0.006), (0, AX_Y - r_in + 0.0026, LENS_Z + 0.006), 0.0006, 8, "emitter", bevel=0)
    marks = [L.text("DG MICRO", (-r_out - 0.0012, AX_Y + 0.004, LENS_Z), 0.0026, (-1, 0, 0), (0, 1, 0), 0.00012, "paint_white")]
    return [tube, saddle, emitter, led] + hoods + marks


def controls():
    out = O.turret((0, AX_Y + 0.0152, LENS_Z - 0.003), (0, 1, 0), 0.0058, 0.0075, marks=8)
    out += O.turret((0.0152, AX_Y, LENS_Z - 0.003), (1, 0, 0), 0.0058, 0.0075, marks=8)
    dial = L.lathe([(0, 0), (0.0078, 0), (0.0078, 0.0055), (0.0068, 0.0065), (0, 0.0065)], (0.0154, AX_Y - 0.006, LENS_Z + 0.008), (1, 0, 0), 40, "aluminium_anodized")
    dial.cut(L.knurl((0.0156, AX_Y - 0.006, LENS_Z + 0.008), (0.022, AX_Y - 0.006, LENS_Z + 0.008), 0.0078, 28, 0.0004))
    nums = [L.text(str(k), (0.0221, AX_Y - 0.006 + 0.0045 * (1 if k % 2 else -1), LENS_Z + 0.008 + 0.002 * (k - 3)), 0.0018, (1, 0, 0), (0, 1, 0), 0.0001, "paint_white")
            for k in range(1, 6)]
    return out + [dial] + nums


def build(ctx):
    it = L.Item("att_red_dot", "attachment")
    it.part("mount").add(*O.rail_clamp(-0.026, 0.02, knob_side=-1), L.box((0, 0.006, -0.003), (0.0105, 0.004, 0.022), 0.0012, 2, "aluminium_anodized"))
    it.part("body").add(*body())
    it.part("controls").add(*controls())
    # The front cap flips up; the eye-side cap is off (its ring stays on the tube): opened about any hinge,
    # its lid stood a few cm from the eye as a large disc beside the window in ADS.
    caps = O.flip_cap((0, AX_Y, LENS_Z - 0.0235), (0, 0, -1), 0.0142, (0, 1, 0), 105) + O.flip_cap((0, AX_Y, LENS_Z + 0.0175), (0, 0, 1), 0.0142, lid=False)
    it.part("caps", mech="lens_caps").add(*caps)
    it.socket("mount", (0, 0, 0))
    it.socket("sight", (0, AX_Y, -0.006))
    it.socket("lens", (0, AX_Y, LENS_Z))
    it.socket("emitter", (0, AX_Y - 0.0098, LENS_Z + 0.006))
    return it
