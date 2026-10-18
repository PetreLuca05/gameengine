"""Baseball bat (data/weapons/05_bat.json): turned ash, 28" (0.72 m, the length of the gameplay box: a
longer bat reaches the camera's near plane in the melee hold), cupped end, flared knob, cloth tape wrap on
the handle (overlapping turns), burned-in brand oval, dents and chips from use. Procedural (path b).
Melee item space: the bat runs along +Y from the grip (origin = the right hand), the left hand at
(0, -0.09, 0) just above the knob."""
import math

import item_lib as L


def body():
    # Lathe profile (radius, distance along +Y from the knob end at y = -0.118).
    prof = [(0.0, 0.0), (0.017, 0.0), (0.0185, 0.004), (0.0175, 0.012), (0.0125, 0.02), (0.0118, 0.06), (0.0122, 0.16), (0.0135, 0.24), (0.017, 0.32), (0.023, 0.4),
            (0.0285, 0.47), (0.0318, 0.54), (0.0332, 0.62), (0.0335, 0.69), (0.0328, 0.705), (0.031, 0.712), (0.022, 0.716), (0.0, 0.708)]
    b = L.lathe(prof, (0, -0.118, 0), (0, 1, 0), 56, "wood_ash")
    # Cupped end + dents / chips along the barrel (the zombie-season wear).
    b.cut(L.sphere((0, 0.716, 0), 0.016, 24, 12, "wood_ash"))
    dents = []
    rnd = [0.13, 0.71, 0.33, 0.92, 0.48, 0.05, 0.62, 0.27, 0.84, 0.4]
    for k in range(10):
        a = rnd[k] * 2 * math.pi
        y = 0.43 + 0.025 * k
        r = 0.0318 + 0.0012 * (k % 3)
        dents.append(L.sphere((math.cos(a) * (r + 0.0018), y, math.sin(a) * (r + 0.0018)), 0.0032 + 0.001 * (k % 2), 10, 6, "wood_ash"))
    b.cut(L.merge(*dents))
    return b


def tape():
    # Cloth tape: overlapping helical turns as slightly proud rings on the handle (y -0.1 .. 0.07).
    out = []
    n = 26
    for k in range(n):
        y = -0.098 + k * 0.0066
        r = 0.0121 + 0.00003 * k
        ring = L.ring_lathe([(r - 0.0003, 0), (r + 0.0009, 0.0006), (r + 0.0009, 0.0058), (r - 0.0003, 0.0068)], (0, y, 0), (0.0, 1.0, 0.035), 40, "fabric_canvas")
        out.append(ring)
    end = L.ring_lathe([(0.0119, 0), (0.0134, 0.0005), (0.0134, 0.01), (0.0119, 0.011)], (0, 0.073, 0), (0, 1, 0), 40, "fabric_canvas")
    return out + [end]


def brand():
    # Burned-in oval brand on the barrel (the "label" side faces +X).
    out = [L.text("DEADGROUND", (0.0313, 0.5, 0.0), 0.0085, (1, 0, 0.0), (0, 1, 0), 0.0003, "wood_walnut"),
           L.text("SLUGGER  34", (0.0323, 0.535, 0.0), 0.006, (1, 0, 0.0), (0, 1, 0), 0.0003, "wood_walnut")]
    return out


K = 0.865   # the turned profile below is a 33" bat: shortened along its axis to 28" (radii unchanged)


def build(ctx):
    it = L.Item("bat", "weapon")
    for name, solids, detail in (("bat", [body()], False), ("grip_tape", tape(), False), ("brand", brand(), True)):
        for s in solids:
            s.scale((1.0, K, 1.0))
        it.part(name, detail=detail).add(*solids)
    it.socket("grip_r", (0, 0, 0))
    it.socket("grip_l", (0, -0.09, 0.0))
    it.socket("tip", (0, 0.716 * K, 0))
    it.socket("sweet_spot", (0, 0.6 * K, 0))
    return it
