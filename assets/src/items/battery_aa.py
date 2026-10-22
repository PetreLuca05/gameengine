"""AA battery (data/items.json "battery_aa"): an LR6 alkaline cell, steel can with the printed wrap, the
positive pip and the flat negative end. Replaces the battery of goggles or a thermal scope. Palm-held.
Procedural (path b)."""
import item_lib as L

R, H = 0.0072, 0.0505


def build(ctx):
    it = L.Item("battery_aa", "item")
    can = L.lathe([(0, 0), (R - 0.0008, 0), (R, 0.0007), (R, H - 0.0007), (R - 0.0008, H), (0.0027, H), (0.0027, H + 0.0011), (0.002, H + 0.0013), (0, H + 0.0013)], (0, 0, 0), (0, 1, 0),
                  40, "steel_bare")
    wrap = L.ring_lathe([(R + 0.00004, 0), (R + 0.00022, 0), (R + 0.00022, H - 0.0016), (R + 0.00004, H - 0.0016)], (0, 0.0008, 0), (0, 1, 0), 40, "label_black")
    top = L.ring_lathe([(R + 0.00022, 0), (R + 0.0003, 0), (R + 0.0003, 0.012), (R + 0.00022, 0.012)], (0, H - 0.0128, 0), (0, 1, 0), 40, "copper")
    txt = [L.text("DEADGROUND", (0, H * 0.5, -R - 0.0004), 0.0021, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("AA LR6 1.5V", (0, H * 0.3, -R - 0.0004), 0.0021, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("+", (0, H - 0.006, -R - 0.0004), 0.004, (0, 0, -1), (0, 1, 0), 0.0001, "label_black")]
    it.part("cell").add(can, wrap, top, *txt)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
