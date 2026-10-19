"""Battery (data/items.json "battery"): a D cell: steel can with the printed wrap (copper top band), the
positive pip and the flat negative end. Palm-held. Procedural (path b)."""
import item_lib as L

R, H = 0.0171, 0.0615


def build(ctx):
    it = L.Item("battery", "item")
    can = L.lathe([(0, 0), (R - 0.0015, 0), (R, 0.0012), (R, H - 0.0012), (R - 0.0015, H), (0.0055, H), (0.0055, H + 0.0015), (0.004, H + 0.0018), (0, H + 0.0018)], (0, 0, 0), (0, 1, 0),
                  56, "steel_bare")
    wrap = L.ring_lathe([(R + 0.00005, 0), (R + 0.0003, 0), (R + 0.0003, H - 0.0024), (R + 0.00005, H - 0.0024)], (0, 0.0012, 0), (0, 1, 0), 56, "label_black")
    top = L.ring_lathe([(R + 0.0003, 0), (R + 0.0004, 0), (R + 0.0004, 0.016), (R + 0.0003, 0.016)], (0, H - 0.0175, 0), (0, 1, 0), 56, "copper")
    txt = [L.text("DEADGROUND  ALKALINE", (0, H * 0.48, -R - 0.0006), 0.0034, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("D  LR20  1.5V", (0, H * 0.33, -R - 0.0006), 0.0045, (0, 0, -1), (0, 1, 0), 0.0001, "label_white"),
           L.text("+", (0, H - 0.008, -R - 0.0006), 0.006, (0, 0, -1), (0, 1, 0), 0.0001, "label_black")]
    it.part("cell").add(can, wrap, top, *txt)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
