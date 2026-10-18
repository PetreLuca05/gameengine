"""Duct tape (data/items.json "duct_tape"): a roll of grey cloth tape on its cardboard core, the tape
end lifted, fibre ribs on the face. Palm-held. Procedural (path b)."""
import item_lib as L


def build(ctx):
    it = L.Item("duct_tape", "item")
    r0, r1, w = 0.038, 0.052, 0.048
    core = L.ring_lathe([(r0 - 0.003, 0), (r0, 0), (r0, w), (r0 - 0.003, w)], (0, 0, 0), (0, 1, 0), 64, "cardboard")
    tape = L.ring_lathe([(r0, 0.0003), (r1, 0.0003), (r1, w - 0.0003), (r0, w - 0.0003)], (0, 0, 0), (0, 1, 0), 64, "tape_grey")
    tape.cut(L.merge(*[L.torus((0, 0.002 + k * 0.0022, 0), (0, 1, 0), r1, 0.00025, 64, 4) for k in range(21)]))
    flap = L.box((0.0, w * 0.5, -r1 - 0.012), (w * 0.5 - 0.0005, 0.0004, 0.012), 0.0002, 1, "tape_grey")
    flap.rotate((0, 1, 0), 90, (0, w * 0.5, 0))
    flap.rotate((0, 0, 1), 0)
    print_ = L.text("DEADGROUND  DUCT 48mm x 25m", (0, w + 0.0002, -(r0 + r1) * 0.5), 0.003, (0, 1, 0), (1, 0, 0), 0.0001, "label_black")
    it.part("roll").add(core, tape, print_)
    it.part("end", mech="end", detail=True).add(flap)
    it.socket("grip_r", (0, 0, 0))
    it.hold_small()
    return it
