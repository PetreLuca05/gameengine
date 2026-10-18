"""Muzzle brake (assets/attachments/muzzle_brake.json): 6.5 cm two-baffle radial brake: wide side baffles,
large side ports, top bleed holes, closed bottom. Procedural (path b). Muzzle at z -0.065."""
import math

import _muzzle as M
import item_lib as L

R = 0.0145
LEN = 0.065


def build(ctx):
    it = L.Item("att_muzzle_brake", "attachment")
    body = L.lathe([(0.0068, 0), (0.0125, 0), (0.0125, 0.012), (R, 0.014), (R, LEN - 0.002), (R - 0.0012, LEN), (0.0068, LEN)], (0, 0, 0), (0, 0, -1), 56, "steel_parkerized",
                   bevel=0.0005)
    cuts = [M.bore(LEN, 0.0052)]
    for z in (-0.026, -0.046):
        cuts.append(L.box((0, 0.0005, z), (0.03, 0.0072, 0.0068), 0, 0))
    for k in range(4):
        cuts.append(L.cyl((0.0035 * (k - 1.5), R + 0.002, -0.058), (0.0035 * (k - 1.5), 0.0, -0.058), 0.0011, 10, bevel=0))
    body.cut(*cuts)
    fins = [L.box((sx * 0.0165, 0, z), (0.0022, 0.004, 0.0028), 0.0008, 2, "steel_parkerized") for z in (-0.017, -0.036, -0.056) for sx in (1, -1)]
    for sx in (1, -1):
        body.cut(L.box((sx * (0.0125 + 0.0003), 0, -0.006), (0.0012, 0.0125, 0.004), 0, 0))
    nut = L.ring_lathe([(0.0073, 0), (0.0138, 0), (0.0138, 0.005), (0.0073, 0.005)], (0, 0, 0.005), (0, 0, -1), 6, "steel_dark")
    it.part("body").add(body, nut, *fins)
    it.socket("mount", (0, 0, 0))
    it.socket("muzzle", (0, 0, -LEN))
    return it
