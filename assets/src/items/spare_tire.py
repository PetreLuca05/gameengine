"""Spare tire (data/items.json "spare_tire"): a 15" steel-wheel space saver: tread blocks and sipes,
sidewall lettering and rim flange, pressed steel wheel with the hub, five lug holes and cooling slots.
Carried by the tread (grip_r on top). Procedural (path b)."""
import math

import item_lib as L

R, W = 0.29, 0.13


def build(ctx):
    it = L.Item("spare_tire", "item")
    c = (0, -R, 0)
    # Section profile (radius, axial) with the three circumferential grooves cut into the tread.
    prof = [(0.19, -W * 0.5), (0.24, -W * 0.5), (0.275, -W * 0.46), (R, -W * 0.36)]
    for gx in (-0.03, 0.0, 0.03):
        prof += [(R, gx - 0.0055), (R - 0.006, gx - 0.0035), (R - 0.006, gx + 0.0035), (R, gx + 0.0055)]
    prof += [(R, W * 0.36), (0.275, W * 0.46), (0.24, W * 0.5), (0.19, W * 0.5), (0.19, W * 0.42), (0.2, W * 0.38), (0.2, -W * 0.38), (0.19, -W * 0.42)]
    tire = L.ring_lathe(prof, (0.0, -R, 0), (1, 0, 0), 96, "rubber")
    # Sipes: shallow cross cuts between the grooves (one merged cutter).
    sipes = []
    for k in range(48):
        a = 2 * math.pi * k / 48
        for (x0, x1) in ((-0.06, -0.036), (-0.024, -0.006), (0.006, 0.024), (0.036, 0.06)):
            b = L.box(((x0 + x1) * 0.5, 0.0, 0), ((x1 - x0) * 0.5, 0.0035, 0.0011), 0, 0)   # on the tread top (y = 0)
            b.rotate((1, 0, 0), math.degrees(a), (0, -R, 0))
            sipes.append(b)
    tire.cut(L.merge(*sipes))
    letters = []
    for k, ch in enumerate("DEADGROUND  T125/70 R15  TEMPORARY USE ONLY  "):
        a = 2 * math.pi * k / 46
        t = L.text(ch, (W * 0.5 + 0.0002, -R + math.cos(a) * 0.245, math.sin(a) * 0.245), 0.012, (1, 0, 0), (0, math.cos(a), math.sin(a)), 0.0006, "rubber")
        letters.append(t)
    wheel = L.ring_lathe([(0.0, -0.002), (0.19, -0.058), (0.195, -0.06), (0.195, 0.06), (0.19, 0.058), (0.12, 0.03), (0.08, 0.008), (0.05, 0.006), (0.0, 0.006)],
                         (0.0, -R, 0), (1, 0, 0), 96, "paint_black")
    holes = [L.cyl((0.02, -R + math.cos(k * 1.2566) * 0.0575, math.sin(k * 1.2566) * 0.0575), (-0.02, -R + math.cos(k * 1.2566) * 0.0575, math.sin(k * 1.2566) * 0.0575),
                   0.0075, 16, bevel=0) for k in range(5)]
    slots = [L.box((0.03, -R + math.cos(k * 0.785) * 0.13, math.sin(k * 0.785) * 0.13), (0.04, 0.014, 0.028), 0, 0).rotate((1, 0, 0), -math.degrees(k * 0.785) + 90,
                                                                                                                        (0.03, -R + math.cos(k * 0.785) * 0.13, math.sin(k * 0.785) * 0.13))
             for k in range(8)]
    wheel.cut(L.cyl((0.02, -R, 0), (-0.02, -R, 0), 0.03, 32, bevel=0), L.merge(*holes), L.merge(*slots))
    valve = L.cyl((-0.04, -R + 0.17, 0.0), (-0.07, -R + 0.168, 0.0), 0.004, 12, "rubber", bevel=0.0005)
    it.part("tire").add(tire, *letters)
    it.part("wheel").add(wheel, valve)
    it.socket("grip_r", (0, 0, 0))
    it.socket("hub", (0, -R, 0))
    return it
