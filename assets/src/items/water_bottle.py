"""Water bottle (data/items.json "water_bottle"): a 0.5 l PET bottle: petaloid base, grip ribs, a blue
wrap label with print, the threaded neck with its tamper ring and the ribbed blue cap (separate).
Palm-held. Procedural (path b)."""
import math

import item_lib as L

R = 0.0325


def build(ctx):
    it = L.Item("water_bottle", "item")
    prof = [(0, 0.006), (R * 0.5, 0.0035), (R * 0.85, 0.0), (R, 0.008), (R, 0.06), (R * 0.93, 0.07), (R, 0.08), (R, 0.135), (R * 0.9, 0.155), (R * 0.6, 0.175), (0.0135, 0.188),
            (0.0135, 0.195), (0.0158, 0.196), (0.0158, 0.199), (0.0135, 0.2), (0.0125, 0.214), (0, 0.214)]
    b = L.lathe(prof, (0, 0, 0), (0, 1, 0), 64, "plastic_clear")
    feet = L.merge(*[L.sphere((math.cos(k * 1.2566) * R * 0.62, 0.003, math.sin(k * 1.2566) * R * 0.62), 0.0085, 12, 6, "plastic_clear") for k in range(5)])
    ribs = L.merge(*[L.torus((0, 0.02 + k * 0.009, 0), (0, 1, 0), R, 0.0008, 64, 6, "plastic_clear") for k in range(5)])
    label = L.ring_lathe([(R + 0.0001, 0), (R + 0.0004, 0), (R + 0.0004, 0.05), (R + 0.0001, 0.05)], (0, 0.082, 0), (0, 1, 0), 64, "label_blue")
    txt = [L.text("PURE", (0, 0.112, -R - 0.0006), 0.008, (0, 0, -1), (0, 1, 0), 0.00012, "label_white"),
           L.text("SPRING WATER  0.5 L", (0, 0.098, -R - 0.0006), 0.0034, (0, 0, -1), (0, 1, 0), 0.0001, "label_white")]
    thread = L.merge(*[L.torus((0, 0.203 + k * 0.0025, 0), (0, 1, 0), 0.0127, 0.0004, 40, 6, "plastic_clear") for k in range(3)])
    cap = L.lathe([(0, 0), (0.0148, 0), (0.0152, 0.002), (0.0152, 0.0145), (0.0142, 0.016), (0, 0.016)], (0, 0.2, 0), (0, 1, 0), 56, "plastic_blue")
    cap.cut(L.knurl((0, 0.2005, 0), (0, 0.2148, 0), 0.0152, 48, 0.0004))
    it.part("bottle").add(b, feet, ribs, label, thread, *txt)
    it.part("cap", mech="cap").add(cap)
    it.socket("grip_r", (0, 0, 0))
    it.socket("cap", (0, 0.216, 0))
    it.hold_small()
    return it
