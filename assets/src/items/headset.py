"""Headset (data/items.json "headset"): an active hearing-protection headset: two olive ear cups (shells with
the leatherette cushions, the volume knobs, the battery door), the boom-free headband with its padded
top and sliding steel arms. Real size 18 x 19 x 8 cm, palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("headset", "item")
    half = 0.072   # the cups' inner faces from the centre
    cups = []
    for sx in (1, -1):
        x = sx * half
        shell = L.lathe([(0, 0), (0.036, 0), (0.04, 0.006), (0.041, 0.024), (0.036, 0.034), (0.022, 0.04), (0, 0.04)], (x, 0, 0), (sx, 0, 0), 48, "polymer_od")
        shell.scale((1.0, 1.25, 1.0), (x, 0, 0))
        cushion = L.torus((x - sx * 0.002, 0, 0), (1, 0, 0), 0.028, 0.009, 48, 12, "rubber")
        cushion.scale((1.0, 1.25, 1.0), (x, 0, 0))
        knob = L.cyl((x + sx * 0.03, -0.028, 0.018), (x + sx * 0.03, -0.028, 0.03), 0.006, 20, "plastic_black", bevel=0.001)
        door = L.box((x + sx * 0.041, -0.01, 0), (0.002, 0.016, 0.014), 0.0015, 2, "polymer_matte")
        mic = L.cyl((x + sx * 0.03, 0.012, -0.034), (x + sx * 0.03, 0.012, -0.04), 0.004, 16, "steel_dark", bevel=0.0006)
        arm = L.box((x + sx * 0.022, 0.085, 0), (0.0025, 0.04, 0.007), 0.001, 2, "steel_stainless")
        cups += [shell, cushion, knob, door, mic, arm]
    band = L.torus((0, 0.11, 0), (0, 0, 1), 0.095, 0.006, 48, 8, "steel_stainless")
    band.scale((1.0, 0.75, 1.0), (0, 0.11, 0))
    band.cut(L.box((0, 0.05, 0), (0.2, 0.07, 0.05), 0, 0))
    pad = L.torus((0, 0.11, 0), (0, 0, 1), 0.098, 0.011, 48, 10, "rubber")
    pad.scale((1.0, 0.75, 1.4), (0, 0.11, 0))
    pad.cut(L.box((0, 0.07, 0), (0.2, 0.095, 0.05), 0, 0))   # only the crown keeps its pad
    label = L.text("DG COMMS", (0.0, 0.1915, 0.0), 0.006, (0, 1, 0), (0, 0, -1), 0.0002, "label_white")
    it.part("cups").add(*cups)
    it.part("headband", mech="band").add(band, pad, label)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
