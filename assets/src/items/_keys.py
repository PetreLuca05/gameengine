"""Keys on a ring with a coloured tag (key_red / key_blue)."""
import math

import item_lib as L


def key(origin, colour, cuts):
    ox, oy, oz = origin
    head = L.top_prism([(ox - 0.012, oz - 0.0), (ox + 0.012, oz - 0.0), (ox + 0.014, oz + 0.012), (ox + 0.008, oz + 0.022), (ox - 0.008, oz + 0.022), (ox - 0.014, oz + 0.012)],
                       oy - 0.0012, oy + 0.0012, bevel=0.0005, segs=2, mat="nickel")
    head.cut(L.cyl((ox, oy - 0.005, oz + 0.015), (ox, oy + 0.005, oz + 0.015), 0.0028, 20, bevel=0))
    cover = L.top_prism([(ox - 0.0135, oz + 0.0005), (ox + 0.0135, oz + 0.0005), (ox + 0.0155, oz + 0.012), (ox + 0.009, oz + 0.0235), (ox - 0.009, oz + 0.0235), (ox - 0.0155, oz + 0.012)],
                        oy - 0.0022, oy + 0.0022, bevel=0.0012, segs=2, mat=colour)
    cover.cut(L.cyl((ox, oy - 0.005, oz + 0.015), (ox, oy + 0.005, oz + 0.015), 0.0036, 20, bevel=0))
    blade_pts = [(ox - 0.0045, oz), (ox - 0.0045, oz - 0.045), (ox - 0.002, oz - 0.049), (ox + 0.0045, oz - 0.049)]
    x = oz - 0.047
    for d in cuts:   # bitting on the +X edge, from the tip back to the shoulder
        blade_pts.append((ox + 0.0045 - d * 0.0011, x + 0.0015))
        blade_pts.append((ox + 0.0045 - d * 0.0011, x + 0.0045))
        x += 0.0062
    blade_pts += [(ox + 0.0045, oz - 0.0035), (ox + 0.0065, oz - 0.0015), (ox + 0.0065, oz)]
    blade = L.top_prism(list(reversed(blade_pts)), oy - 0.001, oy + 0.001, bevel=0.0002, segs=1, mat="nickel")
    for sy in (1, -1):
        blade.cut(L.box((ox - 0.001, oy + sy * 0.001, oz - 0.025), (0.0007, 0.0004, 0.02), 0, 0))
    return [head, blade], [cover]


def keyring(item_id, colour, label, cuts_a, cuts_b):
    it = L.Item(item_id, "item")
    ring = L.torus((0, 0.0, 0.03), (0, 1, 0), 0.0135, 0.0011, 48, 8, "steel_stainless")
    tag = L.top_prism(L.rounded_rect(0.026, 0.05, 0.006, 6, 0.0, 0.062), -0.0016, 0.0016, bevel=0.0012, segs=2, mat=colour)
    tag.cut(L.cyl((0, -0.005, 0.04), (0, 0.005, 0.04), 0.0028, 20, bevel=0))
    txt = L.text(label, (0, 0.0017, 0.066), 0.0055, (0, 1, 0), (0, 0, 1), 0.0002, "paint_white")
    txt2 = L.text(label, (0, -0.0017, 0.066), 0.0055, (0, -1, 0), (0, 0, 1), 0.0002, "paint_white")
    k1, c1 = key((0.0, 0.0, 0.0), colour, cuts_a)
    k2, c2 = key((0.0, 0.003, 0.0), "plastic_black", cuts_b)
    for s in k2 + c2:
        s.rotate((0, 1, 0), 38, (0, 0, 0.03))
    for s in k1 + c1:
        s.rotate((0, 1, 0), -12, (0, 0, 0.03))
    it.part("ring").add(ring)
    it.part("tag", mech="tag").add(tag, txt, txt2)
    it.part("key").add(*k1, *c1)
    it.part("key_spare", detail=True).add(*k2, *c2)
    it.socket("grip_r", (0, 0, 0))
    it.socket("key_tip", (0, 0, -0.049))
    it.hold_small()
    return it
