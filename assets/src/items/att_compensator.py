"""Compensator (assets/attachments/compensator.json): 5 cm three-chamber comp: side ports angled up,
three top ports, a closed bottom, wrench flats, crush washer. Procedural (path b). Muzzle at z -0.05."""
import math

import _muzzle as M
import item_lib as L

R = 0.0138
LEN = 0.05


def build(ctx):
    it = L.Item("att_compensator", "attachment")
    body = L.lathe([(0.0068, 0), (R - 0.0006, 0), (R, 0.001), (R, LEN - 0.002), (R - 0.0012, LEN), (0.0068, LEN)], (0, 0, 0), (0, 0, -1), 56, "steel_parkerized", bevel=0.0004)
    body.cut(M.bore(LEN, 0.0046))
    cuts = []
    for k in range(3):
        z = -0.014 - k * 0.012
        for sx in (1, -1):
            c = L.box((sx * R, 0.003, z), (0.008, 0.0042, 0.0032), 0, 0)
            c.rotate((0, 0, 1), sx * 15, (sx * R, 0.003, z))
            cuts.append(c)
        cuts.append(L.cyl((0, R + 0.002, z), (0, 0.002, z), 0.0018, 14, bevel=0))
    body.cut(L.merge(*cuts))
    for sx in (1, -1):
        body.cut(L.box((sx * (R + 0.0003), 0, -0.004), (0.0012, R, 0.0032), 0, 0))
    washer = L.ring_lathe([(0.0073, 0), (0.0115, 0), (0.0115, 0.0018), (0.0073, 0.0018)], (0, 0, 0.0018), (0, 0, -1), 40, "steel_dark")
    it.part("body").add(body, washer, L.text("DG COMP", (0, -R, -0.03), 0.0026, (0, -1, 0), (0, 0, -1), 0.0001, "paint_white"))
    it.socket("mount", (0, 0, 0))
    it.socket("muzzle", (0, 0, -LEN))
    return it
