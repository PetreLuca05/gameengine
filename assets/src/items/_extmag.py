"""Extended magazines (attachment space: origin = the weapon's magazine slot; the well centre is 3.5 mm
behind it, the feed lips 4 mm above). One per magazine well (the attachment's "mesh_for" variants)."""
import _parts as P
import item_lib as L


def build_mag(item_id, mag_len, drop, half_w, curve, mat, window=True, label=None):
    it = L.Item(item_id, "attachment")
    hl = mag_len * 0.5
    body, plate, follower, case, bullet = P.stanag_mag(0.004, 0.004 - drop, -0.0035 - hl + 0.0003, -0.0035 + hl - 0.0003, half_w=half_w, curve=curve, mat=mat, windows=window)
    # A grip extension ("+10") on the floorplate and a pull loop.
    fz = -0.0035 - curve
    ext = L.side_prism([(fz - hl, 0.004 - drop - 0.006), (fz + hl, 0.004 - drop - 0.006), (fz + hl - 0.002, 0.004 - drop - 0.016), (fz - hl + 0.003, 0.004 - drop - 0.016)],
                       -half_w - 0.002, half_w + 0.002, bevel=0.0025, segs=3, mat=mat)
    loop = L.torus((0, 0.004 - drop - 0.02, fz), (1, 0, 0), 0.008, 0.0016, 24, 8, "webbing_black")
    parts = [body, plate, ext, loop]
    if label:
        parts.append(L.text(label, (half_w + 0.0002, 0.004 - drop * 0.55, -0.0035 - curve * 0.55), 0.004, (1, 0, 0), (0, 1, 0), 0.0002, "paint_white"))
    it.part("magazine", mech="mag").add(*parts)
    it.part("follower").add(follower)
    it.part("top_round", detail=True).add(case, bullet)
    it.socket("mount", (0, 0, 0))
    it.socket("floorplate", (0, 0.004 - drop - 0.016, fz))
    return it
