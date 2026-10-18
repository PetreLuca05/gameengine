"""Muzzle devices (attachment space: origin = the barrel tip the device threads onto, -Z = the bore)."""
import math

import item_lib as L


def thread_collar(r, length, mat):
    """The rear section that threads onto the barrel: wrench flats, crush-washer seat."""
    c = L.lathe([(0.0068, 0), (r * 0.92, 0), (r, 0.0015), (r, length), (0.0068, length)], (0, 0, 0.0), (0, 0, -1), 48, mat, bevel=0.0003)
    for sx in (1, -1):
        c.cut(L.box((sx * (r + 0.0003), 0, -length * 0.5), (0.0012, r, length * 0.42), 0, 0))
    return c


def bore(length, r=0.0035):
    return L.cyl((0, 0, 0.002), (0, 0, -length - 0.002), r, 24, bevel=0)


def marks(text, r, z):
    t = L.text(text, (r, 0.0, z), 0.0025, (1, 0, 0), (0, 0, -1), 0.0001, "paint_white")
    return t
