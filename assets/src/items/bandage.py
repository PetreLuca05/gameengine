"""Bandage (data/items.json "bandage"): an elastic bandage roll: wound layers (gauze, a woven edge
stripe), the loose tail with its two metal clips, half the paper wrapper torn back. Palm-held.
Procedural (path b)."""
import math

import item_lib as L


def build(ctx):
    it = L.Item("bandage", "item")
    r, w = 0.028, 0.07
    core = L.ring_lathe([(0.008, 0), (0.0095, 0), (0.0095, w), (0.008, w)], (-w * 0.5, 0, 0), (1, 0, 0), 32, "cardboard")
    layers = []
    for k in range(6):
        rr = 0.0095 + (r - 0.0095) * (k + 1) / 6.0
        layers.append(L.ring_lathe([(rr - 0.0032, 0.0004), (rr, 0.0006), (rr, w - 0.0006), (rr - 0.0032, w - 0.0004)], (-w * 0.5, 0, 0), (1, 0, 0), 48, "fabric_gauze"))
    stripe = [L.torus((x, 0, 0), (1, 0, 0), r + 0.0002, 0.0005, 48, 6, "label_blue") for x in (-w * 0.5 + 0.004, w * 0.5 - 0.004)]
    tail = L.box((0, -r + 0.0006, 0.022), (w * 0.5 - 0.001, 0.0006, 0.022), 0.0004, 1, "fabric_gauze")
    tail.rotate((1, 0, 0), -12, (0, -r, 0.0))
    clips = []
    for x in (-0.015, 0.015):
        c = L.side_prism([(0.03, -r - 0.001), (0.046, -r - 0.001), (0.046, -r + 0.0015), (0.03, -r + 0.0015)], x - 0.004, x + 0.004, bevel=0.0004, segs=1, mat="nickel")
        c.cut(L.merge(*[L.box((x, -r, 0.033 + k * 0.004), (0.0045, 0.003, 0.0006), 0, 0) for k in range(3)]))
        clips.append(c)
    wrap = L.ring_lathe([(r + 0.0004, 0), (r + 0.0011, 0), (r + 0.0011, w * 0.45), (r + 0.0004, w * 0.45)], (w * 0.05, 0, 0), (1, 0, 0), 48, "paper_label")
    wrap.cut(L.box((0, r * 0.8, -r * 0.5), (w, r * 0.6, r * 0.9), 0, 0))
    print_ = L.text("ELASTIC  10cm", (w * 0.27, r + 0.0012, 0.0), 0.0045, (0, 1, 0), (1, 0, 0), 0.0001, "label_blue")
    it.part("roll").add(core, *layers, *stripe, wrap, print_)
    it.part("tail", mech="tail").add(tail, *clips)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
