"""Fishing kit (data/items.json "fishing_kit"): a green tackle box with the fold-down handle, two latches
and the lid (separate, hinged), a lure and hooks clipped on top. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("fishing_kit", "item")
    w, h, d = 0.24, 0.07, 0.14
    base = L.box((0, h * 0.35, 0), (w * 0.5, h * 0.35, d * 0.5), 0.006, 3, "plastic_green")
    lid = L.box((0, h * 0.85, 0), (w * 0.5 + 0.001, h * 0.15, d * 0.5 + 0.001), 0.006, 3, "plastic_green")
    lid.cut(L.merge(*[L.box((x, h + 0.0005, 0), (0.035, 0.001, d * 0.5 - 0.012), 0.002, 1) for x in (-0.06, 0.06)]))
    latches = [L.box((x, h * 0.7, d * 0.5 + 0.003), (0.011, 0.012, 0.003), 0.0015, 2, "plastic_black") for x in (-0.07, 0.07)]
    hinge = L.cyl((-w * 0.5 + 0.01, h * 0.7, -d * 0.5 - 0.002), (w * 0.5 - 0.01, h * 0.7, -d * 0.5 - 0.002), 0.0032, 16, "plastic_green", bevel=0.0004)
    handle = L.box((0, h + 0.006, 0.0), (0.045, 0.004, 0.008), 0.003, 3, "plastic_black")
    lure = L.lathe([(0, 0), (0.005, 0.006), (0.0065, 0.02), (0.004, 0.032), (0, 0.036)], (0.03, h + 0.006, -0.03), (1, 0, 0), 16, "paint_yellow")
    hook = L.torus((0.07, h + 0.006, -0.03), (0, 1, 0), 0.006, 0.0006, 20, 4, "steel_bare")
    txt = L.text("DEADGROUND OUTDOORS", (0, h * 0.35, d * 0.5 + 0.0004), 0.007, (0, 0, 1), (0, 1, 0), 0.0002, "paint_white")
    it.part("box").add(base, hinge, txt)
    it.part("lid", pivot=(0, h * 0.7, -d * 0.5), mech="lid").add(lid, *latches, handle, lure, hook)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
