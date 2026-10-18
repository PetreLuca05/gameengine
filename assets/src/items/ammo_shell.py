"""12ga shells (data/items.json "ammo_shell"): a 25-shell box, lid flap open, the red hull shells with
brass heads standing in the tray. Palm-held. Procedural (path b)."""
import _boxes as B
import item_lib as L


def build(ctx):
    w, h, d = 0.105, 0.07, 0.065
    shells = []
    for i in range(5):
        for j in range(2):
            x, z = -w * 0.5 + 0.011 + i * 0.021, -d * 0.5 + 0.012 + j * 0.021 + 0.022
            hull = L.cyl((x, h - 0.028, z), (x, h + 0.012, z), 0.0101, 20, "plastic_red", bevel=0.0006)
            head = L.cyl((x, h - 0.03, z), (x, h - 0.022, z), 0.0106, 20, "brass", bevel=0.0004)
            crimp = L.lathe([(0.0101, 0), (0.006, 0.0025), (0, 0.003)], (x, h + 0.012, z), (0, 1, 0), 20, "plastic_red")
            shells += [hull, head, crimp]
    it = B.ammo_box("ammo_shell", w, h, d, "12 GAUGE", "25 SHELLS  00 BUCK  2 3/4\"", "label_red", shells)
    it.hold_small()
    return it
