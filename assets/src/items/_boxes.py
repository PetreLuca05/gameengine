"""Printed cardboard boxes and other simple props for the cheap tier."""
import item_lib as L


def ammo_box(item_id, w, h, d, title, sub, band, rounds=None):
    """A tuck-flap cardboard ammo box (w x h x d), printed faces, the end flap ajar, rounds showing."""
    it = L.Item(item_id, "item")
    box = L.box((0, h * 0.5, 0), (w * 0.5, h * 0.5, d * 0.5), 0.0012, 2, "cardboard")
    stripe = L.box((0, h * 0.5, d * 0.5 + 0.0003), (w * 0.5 - 0.002, h * 0.18, 0.0003), 0.0003, 1, band)
    stripe2 = L.box((0, h * 0.5, -d * 0.5 - 0.0003), (w * 0.5 - 0.002, h * 0.18, 0.0003), 0.0003, 1, band)
    txt = [L.text(title, (0, h * 0.5, d * 0.5 + 0.0007), h * 0.2, (0, 0, 1), (0, 1, 0), 0.0001, "label_white"),
           L.text(sub, (0, h * 0.18, d * 0.5 + 0.0004), h * 0.09, (0, 0, 1), (0, 1, 0), 0.0001, "label_black"),
           L.text(title, (0, h * 0.5, -d * 0.5 - 0.0007), h * 0.2, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("DEADGROUND AMMUNITION", (0, h + 0.0003, 0), min(w, d) * 0.09, (0, 1, 0), (0, 0, -1), 0.0001, "label_black")]
    flap = L.box((w * 0.5 + 0.0006, h * 0.5, 0), (0.0006, h * 0.5 - 0.0005, d * 0.5 - 0.0005), 0.0003, 1, "cardboard")
    flap.rotate((0, 0, 1), -35, (w * 0.5, h, 0))
    it.part("box").add(box, stripe, stripe2, *txt)
    it.part("flap", mech="flap").add(flap)
    if rounds:
        it.part("rounds", detail=True).add(*rounds)
    it.socket("grip_r", (0, 0, 0))
    return it


def cartridge(pos, axis, case_r, case_l, bullet_l, neck=0.7, mat_case="brass", mat_bullet="copper", segs=16):
    p = L.Vector(pos)
    a = L.Vector(axis).normalized()
    c = L.lathe([(0, 0), (case_r, 0), (case_r, case_l * 0.82), (case_r * neck, case_l * 0.9), (case_r * neck, case_l), (0, case_l)], p, a, segs, mat_case)
    b = L.lathe([(case_r * neck * 0.95, 0), (case_r * neck * 0.95, bullet_l * 0.35), (case_r * 0.45, bullet_l * 0.8), (0, bullet_l)], p + a * case_l, a, segs, mat_bullet)
    return [c, b]
