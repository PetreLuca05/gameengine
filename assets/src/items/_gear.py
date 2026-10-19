"""Shared helpers for the grid inventory's gear (wearables, bags, cases: docs/INVENTORY.md)."""
from mathutils import Vector


def face_front(it):
    """Turns a finished item half a turn about +Y through the origin (grip_r stays put), so the face modelled
    at +Z (pockets, pouches, labels, latches) looks down -Z: the side the icon / cell camera and a held
    item show (docs/ITEM_PIPELINE.md conventions)."""
    for p in it.parts:
        p.pivot = Vector((-p.pivot.x, p.pivot.y, -p.pivot.z))
        for so in p.solids:
            so.rotate((0, 1, 0), 180.0)
    it.sockets = [(n, pt, Vector((-pos.x, pos.y, -pos.z)), Vector((rot.x, (rot.y + 180.0) % 360.0, rot.z))) for (n, pt, pos, rot) in it.sockets]
    return it
