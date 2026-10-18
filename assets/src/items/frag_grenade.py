"""Frag grenade (data/items.json "frag_grenade"): M67 pattern: olive steel sphere with its seam and the
yellow lot band, fuze body (threaded collar), the spoon (separate: it flies off on release), the safety
pin with its pull ring (separate: pulled when cooking) and the safety clip. Palm-held.
Procedural (path b)."""
import math

import item_lib as L

R = 0.032


def build(ctx):
    it = L.Item("frag_grenade", "item")
    body = L.lathe([(0, 0), (R * 0.55, 0.002), (R * 0.88, 0.012), (R, 0.03), (R * 0.97, 0.044), (R * 0.85, 0.056), (R * 0.62, 0.064), (0.0118, 0.0665), (0.0118, 0.068),
                    (0, 0.068)], (0, 0, 0), (0, 1, 0), 56, "paint_olive")
    seam = L.torus((0, 0.034, 0), (0, 1, 0), R * 0.995, 0.0006, 56, 6, "paint_olive")
    band = L.ring_lathe([(R * 0.97, 0), (R * 0.985, 0), (R * 0.985, 0.006), (R * 0.97, 0.006)], (0, 0.045, 0), (0, 1, 0), 56, "paint_yellow")
    lot = L.text("GRENADE HAND FRAG DELAY M67  LOT 22-117", (0, 0.028, -R - 0.0002), 0.0021, (0, 0, -1), (0, 1, 0), 0.0001, "paint_yellow")
    fuze = L.lathe([(0, 0), (0.0118, 0), (0.0122, 0.002), (0.0122, 0.008), (0.009, 0.01), (0.009, 0.026), (0.006, 0.028), (0, 0.028)], (0, 0.066, 0), (0, 1, 0), 40,
                   "steel_dark")
    fuze.cut(L.merge(*[L.torus((0, 0.0685 + k * 0.0012, 0), (0, 1, 0), 0.0122, 0.00035, 40, 6) for k in range(4)]))
    spoon = L.side_prism([(-0.004, 0.094), (0.004, 0.094), (0.012, 0.09), (0.03, 0.072), (0.037, 0.05), (0.04, 0.025), (0.036, 0.024), (0.033, 0.049), (0.027, 0.068),
                          (0.011, 0.086), (0.004, 0.0905), (-0.004, 0.0905)], -0.006, 0.006, bevel=0.0006, segs=2, mat="paint_olive", angle=25)
    spoon.rotate((0, 1, 0), 90)
    pin = L.cyl((-0.016, 0.086, 0.0), (0.016, 0.086, 0.0), 0.0011, 12, "steel_stainless", bevel=0.0002)
    ring = L.torus((-0.026, 0.086, 0.0), (0, 0, 1), 0.0115, 0.0012, 36, 8, "steel_stainless")
    clip = L.side_prism([(-0.002, 0.075), (0.002, 0.075), (0.002, 0.096), (-0.002, 0.096)], 0.009, 0.0105, bevel=0.0003, segs=1, mat="steel_stainless")
    it.part("body").add(body, seam, band, lot)
    it.part("fuze").add(fuze, clip)
    it.part("spoon", pivot=(0, 0.09, 0.0), mech="spoon").add(spoon)
    it.part("pin_ring", mech="pin").add(pin, ring)
    it.socket("grip_r", (0, 0, 0))
    it.socket("pin", (-0.026, 0.086, 0.0))
    it.socket("spoon", (0, 0.09, 0.0))
    it.hold_small()
    return it
