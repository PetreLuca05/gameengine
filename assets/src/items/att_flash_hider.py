"""Flash hider (assets/attachments/flash_hider.json): 5.5 cm three-prong hider with a tapered body,
wrench flats and crush washer. Procedural (path b). Muzzle at z -0.055."""
import math

import _muzzle as M
import item_lib as L

R = 0.0115
LEN = 0.055


def build(ctx):
    it = L.Item("att_flash_hider", "attachment")
    body = L.lathe([(0.0068, 0), (R, 0), (R, 0.018), (R + 0.0008, 0.024), (R + 0.0008, LEN - 0.001), (R, LEN), (0.0068, LEN)], (0, 0, 0), (0, 0, -1), 56, "steel_parkerized",
                   bevel=0.0004)
    body.cut(L.cyl((0, 0, -0.016), (0, 0, -LEN - 0.002), 0.0086, 40, bevel=0))
    body.cut(M.bore(0.02, 0.0045))
    cuts = []
    for k in range(3):
        a = math.radians(90 + k * 120)
        c = L.box((0, 0, 0), (0.0022, 0.012, 0.0165), 0, 0)
        c.rotate((0, 0, 1), math.degrees(a) - 90)
        c.move((math.cos(a) * 0.009, math.sin(a) * 0.009, -LEN + 0.0145))
        cuts.append(c)
    body.cut(L.merge(*cuts))
    for sx in (1, -1):
        body.cut(L.box((sx * (R + 0.0003), 0, -0.006), (0.0012, R, 0.004), 0, 0))
    washer = L.ring_lathe([(0.0073, 0), (0.0115, 0), (0.0115, 0.0018), (0.0073, 0.0018)], (0, 0, 0.0018), (0, 0, -1), 40, "steel_dark")
    it.part("body").add(body, washer)
    it.socket("mount", (0, 0, 0))
    it.socket("muzzle", (0, 0, -LEN))
    return it
