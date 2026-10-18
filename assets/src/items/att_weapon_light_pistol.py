"""Weapon light, pistol rail (assets/attachments/weapon_light.json "mesh_for" pistol): compact TLR-style
light: a squared polymer body under the frame, ambidextrous paddle switches, bezel + LED.
Procedural (path b)."""
import _light as G
import item_lib as L

Y = -0.013


def build(ctx):
    it = L.Item("att_weapon_light_pistol", "attachment")
    clamp = L.box((0, -0.003, 0.0), (0.0112, 0.003, 0.012), 0.0008, 2, "polymer_matte")
    body = L.side_prism([(0.028, -0.004), (-0.04, -0.004), (-0.052, Y + 0.012), (-0.052, Y - 0.012), (0.024, Y - 0.012), (0.03, Y - 0.006)], -0.0145, 0.0145, bevel=0.003,
                        segs=3, mat="polymer_matte", angle=20)
    hd, em = G.head(-0.068, 0.0118, Y, depth=0.017, crenel=False)
    paddles = [L.side_prism([(0.018, Y - 0.002), (0.04, Y - 0.004), (0.04, Y - 0.011), (0.018, Y - 0.01)], sx * 0.0145, sx * 0.0172, bevel=0.0008, segs=2, mat="polymer_matte")
               for sx in (1, -1)]
    it.part("clamp").add(clamp, L.cyl((-0.0125, -0.004, 0.0), (0.0125, -0.004, 0.0), 0.0018, 12, "steel_dark", bevel=0.0002))
    it.part("body").add(body, *hd)
    it.part("switch", mech="switch").add(*paddles)
    it.socket("mount", (0, 0, 0))
    it.socket("emitter", em)
    return it
