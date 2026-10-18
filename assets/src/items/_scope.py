"""Riflescope builder (the 4x / 8x): 30 mm main tube, objective bell with the objective lens recessed behind
a sunshade lip, saddle with elevation / windage turrets (+ a side-focus / illumination knob), power ring and
ocular bell with the ocular lens a few mm forward of the JSON lens plane, internal erector lenses, flip
caps, two rings on a Picatinny base. Lens stack: objective, erector pair, reticle plane (socket), ocular."""
import _optics as O
import item_lib as L


def scope(item_id, j, tube_r=0.015, obj_r=0.023, ring_z=(-0.055, 0.055), base_half=0.075, side_knob=False, label="4X32", eye_r=0.0195):
    ax = j["lens"]["pos"][1]
    oz = j["lens"]["objective"][2]   # objective front (the JSON objective plane)
    ez = j["lens"]["pos"][2]         # ocular lens plane (eye side)
    it = L.Item(item_id, "attachment")
    # Base + rings.
    base = L.box((0, 0.004, 0), (0.0105, 0.004, base_half), 0.0012, 2, "aluminium_anodized")
    clamps = O.rail_clamp(ring_z[0] - 0.012, ring_z[0] + 0.012) + O.rail_clamp(ring_z[1] - 0.012, ring_z[1] + 0.012, knob_side=-1)
    it.part("mount").add(base, *clamps)
    rings = []
    for z in ring_z:
        rings += O.scope_ring(z, ax, tube_r, 0.008)
    it.part("rings").add(*rings)
    # Tube, objective bell, saddle, eyepiece (lathe profiles along +Z from the objective end).
    bell_len = 0.056 if obj_r > 0.024 else 0.05
    eye_len = ez - 0.124 if ez < 0.185 else ez - 0.144
    tube = L.lathe([(0, 0), (obj_r - 0.0008, 0), (obj_r, 0.002), (obj_r, bell_len * 0.62), (tube_r + 0.0006, bell_len), (tube_r, bell_len + 0.002), (tube_r, (ez - oz) - eye_len - 0.004),
                    (tube_r + 0.0008, (ez - oz) - eye_len), (0, (ez - oz) - eye_len)], (0, ax, oz), (0, 0, 1), 64, "aluminium_anodized")
    tube.cut(L.cyl((0, ax, oz - 0.002), (0, ax, oz + 0.006), obj_r - 0.0016, 56, bevel=0))     # sunshade lip
    saddle = L.box((0, ax + 0.003, (ring_z[0] + ring_z[1]) * 0.5), (tube_r + 0.003, tube_r * 0.62, 0.016), 0.003, 3, "aluminium_anodized")
    saddle.cut(L.cyl((0, ax, -0.05), (0, ax, 0.05), tube_r - 0.0001, 48, bevel=0))
    sz = (ring_z[0] + ring_z[1]) * 0.5
    turrets = O.turret((0, ax + tube_r + 0.0045, sz), (0, 1, 0), 0.0092, 0.012) + O.turret((tube_r + 0.0045, ax, sz), (1, 0, 0), 0.0092, 0.012)
    if side_knob:
        turrets += O.turret((-tube_r - 0.0045, ax, sz + 0.002), (-1, 0, 0), 0.0105, 0.009, marks=6)
        turrets.append(L.cyl((-tube_r - 0.0135, ax, sz + 0.002), (-tube_r - 0.0155, ax, sz + 0.002), 0.0045, 24, "plastic_red", bevel=0.0004))   # illumination
    cap_txt = [L.text("UP", (0, ax + tube_r + 0.0167, sz), 0.0028, (0, 1, 0), (0, 0, -1), 0.0001, "paint_white"),
               L.text("R", (tube_r + 0.0167, ax, sz), 0.0028, (1, 0, 0), (0, 1, 0), 0.0001, "paint_white")]
    eyep = L.lathe([(tube_r + 0.0008, 0), (tube_r + 0.002, 0.004), (eye_r - 0.0015, 0.012), (eye_r, eye_len * 0.4), (eye_r, eye_len - 0.003), (eye_r - 0.0012, eye_len),
                    (0.0175 + 0.0004, eye_len), (0.0175 + 0.0004, eye_len - 0.004), (0, eye_len - 0.004)], (0, ax, ez - eye_len), (0, 0, 1), 64, "aluminium_anodized")
    eyep.cut(L.cyl((0, ax, ez - 0.006), (0, ax, ez + 0.003), 0.0175 + 0.0004, 56, bevel=0))   # the eye side stays open down to the lens
    power = L.ring_lathe([(tube_r + 0.0008, 0), (tube_r + 0.0035, 0.0), (tube_r + 0.0035, 0.012), (tube_r + 0.0008, 0.012)], (0, ax, ez - eye_len - 0.001), (0, 0, 1), 56,
                         "rubber")
    power.cut(L.knurl((0, ax, ez - eye_len), (0, ax, ez - eye_len + 0.011), tube_r + 0.0035, 30, 0.0006))
    marks = [L.text("DEADGROUND  " + label, (-tube_r - 0.0002, ax, (oz + ez) * 0.5 - 0.02), 0.0032, (-1, 0, 0), (0, 0, -1), 0.0001, "paint_white")]
    marks[0].rotate((1, 0, 0), 0)
    it.part("body").add(tube, saddle, eyep, power, *(turrets + cap_txt + marks))
    # Lens stack.
    ls = [O.lens((0, ax, oz + 0.008), (0, 0, 1), obj_r - 0.0018), O.lens((0, ax, oz + bell_len + 0.01), (0, 0, 1), tube_r - 0.004),
          O.lens((0, ax, oz + bell_len + 0.028), (0, 0, 1), tube_r - 0.004), O.lens((0, ax, ez - 0.004), (0, 0, 1), 0.0172)]
    it.part("lens_stack").add(*ls)
    caps = O.flip_cap((0, ax, oz - 0.002), (0, 0, -1), obj_r, (0, 1, 0), 110) + O.flip_cap((0, ax, ez + 0.002), (0, 0, 1), eye_r, (0, -1, 0), 100)
    it.part("caps", mech="lens_caps").add(*caps)
    it.socket("mount", (0, 0, 0))
    it.socket("sight", tuple(j["sight_socket"]))
    it.socket("lens", tuple(j["lens"]["pos"]))
    it.socket("objective", tuple(j["lens"]["objective"]))
    it.socket("reticle", (0, ax, ez - eye_len - 0.004))   # second focal plane: the erector's image, ahead of the power ring
    return it
