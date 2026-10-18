"""Molotov (data/items.json "molotov"): a green glass bottle (shoulder, neck, lip, embossed base ring) with a
torn cloth wick stuffed in the neck and tied off with string, the bottle label half peeled. Palm-held,
neck up. Procedural (path b)."""
import math

import item_lib as L

R = 0.033


def build(ctx):
    it = L.Item("molotov", "item")
    bottle = L.lathe([(0, 0.003), (R * 0.8, 0.0015), (R - 0.0015, 0.0), (R, 0.004), (R, 0.13), (R * 0.95, 0.15), (R * 0.68, 0.17), (0.0135, 0.192), (0.0128, 0.21),
                      (0.0135, 0.213), (0.0142, 0.218), (0.0118, 0.219), (0.0098, 0.214), (0.0098, 0.195), (R * 0.66 - 0.002, 0.168), (R - 0.0025, 0.13),
                      (R - 0.0025, 0.007), (0, 0.007)], (0, 0, 0), (0, 1, 0), 64, "glass_green")
    base_ring = L.merge(*[L.box((math.cos(k * 0.5) * (R - 0.004), 0.0012, math.sin(k * 0.5) * (R - 0.004)), (0.0012, 0.0012, 0.0012), 0.0003, 1, "glass_green")
                          for k in range(12)])
    label = L.ring_lathe([(R + 0.00005, 0), (R + 0.0003, 0), (R + 0.0003, 0.06), (R + 0.00005, 0.06)], (0, 0.04, 0), (0, 1, 0), 64, "paper_label")
    label.cut(L.side_prism([(-R - 0.01, 0.07), (-R * 0.2, 0.1), (R * 0.1, 0.12), (R * 0.1, 0.11), (-R * 0.2, 0.09), (-R - 0.01, 0.06)], -0.05, -0.0, 0, 0))
    txt = L.text("CASTLE  VODKA", (0, 0.075, -R - 0.0005), 0.0055, (0, 0, -1), (0, 1, 0), 0.0001, "label_red")
    # Wick: a twisted cloth strip in the neck, flared out of the mouth, string wraps.
    wick = []
    for k in range(10):
        t = k / 9.0
        y = 0.196 + 0.06 * t
        r = 0.0095 - 0.003 * t
        c = L.sphere((0.004 * math.sin(t * 5), y, 0.003 * math.cos(t * 4)), r, 16, 8, "fabric_canvas")
        c.scale((1.0, 1.4, 0.75), (0.004 * math.sin(t * 5), y, 0.003 * math.cos(t * 4)))
        wick.append(c)
    flap = L.box((0.006, 0.258, 0.0), (0.012, 0.008, 0.0012), 0.0008, 1, "fabric_canvas")
    flap.rotate((0, 0, 1), -30, (0.006, 0.252, 0))
    string = [L.torus((0, 0.2 + k * 0.003, 0), (0, 1, 0.05), 0.0142, 0.0007, 32, 6, "fabric_canvas") for k in range(3)]
    it.part("bottle").add(bottle, base_ring, label, txt)
    it.part("wick", mech="wick").add(*wick, flap, *string)
    it.socket("grip_r", (0, 0, 0))
    it.socket("wick", (0, 0.26, 0))
    it.hold_small()
    return it
