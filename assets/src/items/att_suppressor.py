"""Suppressor (assets/attachments/suppressor.json): 38 mm direct-thread can, 17 cm: ratchet QD collar,
tube with laser-engraved markings, flat front cap with wrench slots, the baffle stack visible down the bore
(cone baffles). Procedural (path b). Muzzle (the exit) at z -0.17 (muzzle_length)."""
import math

import _muzzle as M
import item_lib as L

R = 0.019
LEN = 0.17


def build(ctx):
    it = L.Item("att_suppressor", "attachment")
    collar = M.thread_collar(0.0142, 0.022, "steel_cerakote")
    teeth = [L.box((math.cos(a) * 0.0145, math.sin(a) * 0.0145, -0.012), (0.0012, 0.0012, 0.006), 0.0003, 1, "steel_cerakote") for a in
             [k * 2 * math.pi / 16 for k in range(16)]]
    tube = L.lathe([(0.0152, 0), (R - 0.0012, 0), (R, 0.0015), (R, LEN - 0.024), (R - 0.0012, LEN - 0.0225), (0.0145, LEN - 0.0225), (0.0145, LEN - 0.018), (0.0152, LEN - 0.018)],
                   (0, 0, -0.018), (0, 0, -1), 64, "steel_cerakote", bevel=0.0004)
    cap = L.lathe([(0.0042, 0), (R - 0.0008, 0), (R, 0.0012), (R, 0.012), (R - 0.0015, 0.0135), (0.0042, 0.0135)], (0, 0, -LEN + 0.0135), (0, 0, -1), 64, "steel_cerakote")
    for k in range(4):
        a = k * math.pi / 2
        cap.cut(L.box((math.cos(a) * (R - 0.0025), math.sin(a) * (R - 0.0025), -LEN + 0.001), (0.002, 0.002, 0.0025), 0, 0).rotate((0, 0, 1), math.degrees(a)))
    cap.cut(M.bore(0.2, 0.0042))
    # Baffle cones (seen down the bore from the front).
    baffles = [L.ring_lathe([(0.0042, 0), (R - 0.002, 0.008), (R - 0.002, 0.0095), (0.0045, 0.0018)], (0, 0, -0.03 - k * 0.0185), (0, 0, -1), 40, "steel_dark")
               for k in range(7)]
    marks = [L.text("DEADGROUND  OBSIDIAN 38", (R, 0.0, -0.09), 0.0034, (1, 0, 0), (0, 1, 0), 0.0001, "paint_white"),
             L.text("5.56 / 7.62  FULL AUTO", (R * 0.7071, R * 0.7071, -0.09), 0.0026, (0.7071, 0.7071, 0), (-0.7071, 0.7071, 0), 0.0001, "paint_white")]
    for t in marks:
        t.rotate((0, 0, 1), 0)
    it.part("collar").add(collar, *teeth)
    it.part("tube").add(tube, cap, *marks)
    it.part("baffles", detail=True).add(*baffles)
    it.socket("mount", (0, 0, 0))
    it.socket("muzzle", (0, 0, -LEN))
    return it
