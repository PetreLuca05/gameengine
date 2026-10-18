"""5.56 rounds (data/items.json "ammo_rifle"): a 20-round cardboard box, flap open, rounds showing.
Palm-held. Procedural (path b): _boxes.py."""
import _boxes as B


def build(ctx):
    w, h, d = 0.105, 0.03, 0.07
    rounds = []
    for i in range(4):
        rounds += B.cartridge((w * 0.5 - 0.004, h * 0.25 + (i % 2) * 0.01, -d * 0.35 + i * 0.016), (1, 0, 0), 0.0048, 0.045, 0.02, 0.6)
    it = B.ammo_box("ammo_rifle", w, h, d, "5.56x45 NATO", "20 CARTRIDGES  62 GR  M855", "label_green", rounds)
    it.hold_small()
    return it
