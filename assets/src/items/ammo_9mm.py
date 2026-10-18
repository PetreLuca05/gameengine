"""9mm rounds (data/items.json "ammo_9mm"): a 50-round cardboard box with the end flap open and the
rounds in their tray. Palm-held. Procedural (path b): _boxes.py."""
import _boxes as B


def build(ctx):
    w, h, d = 0.075, 0.04, 0.057
    rounds = []
    for i in range(4):
        rounds += B.cartridge((w * 0.5 - 0.004, h * 0.2 + i * 0.008, -d * 0.3 + i * 0.012), (1, 0, 0), 0.0049, 0.019, 0.011, 0.98)
    it = B.ammo_box("ammo_9mm", w, h, d, "9mm LUGER", "50 CARTRIDGES  124 GR FMJ", "label_blue", rounds)
    it.hold_small()
    return it
