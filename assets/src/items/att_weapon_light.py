"""Weapon light (assets/attachments/weapon_light.json, new): 1000-lumen rifle light on a rail clamp under
the handguard: knurled 1" body, crenelated bezel with a smooth reflector and LED, rubber tail-cap
switch. The flashlight (F) shines from its emitter socket. Procedural (path b); the pistol carries the
compact att_weapon_light_pistol ("mesh_for")."""
import _light as G
import item_lib as L

Y = -0.022


def build(ctx):
    it = L.Item("att_weapon_light", "attachment")
    it.part("clamp").add(*G.hanging_clamp(-0.022, 0.02), L.box((0, -0.007, -0.001), (0.0085, 0.004, 0.018), 0.0012, 2, "aluminium_anodized"))
    body = G.knurled_tube(0.04, -0.048, Y, 0.0128, bands=((0.032, 0.012), (-0.02, -0.04)), rings=2)
    neck = L.box((0, (Y - 0.007) * 0.5 - 0.002, -0.001), (0.0065, (-Y - 0.007) * 0.5 - 0.003, 0.016), 0.0012, 2, "aluminium_anodized")
    hd, em = G.head(-0.074, 0.0165, Y)
    tail = L.lathe([(0, 0), (0.0128, 0), (0.0132, 0.003), (0.0132, 0.012), (0.012, 0.014), (0, 0.014)], (0, Y, 0.04), (0, 0, 1), 48, "aluminium_anodized")
    button = L.lathe([(0, 0), (0.0085, 0), (0.0085, 0.003), (0.006, 0.0045), (0, 0.005)], (0, Y, 0.054), (0, 0, 1), 32, "rubber")
    marks = [L.text("DG-X1000", (0.0129, Y, -0.004), 0.0035, (1, 0, 0), (0, 1, 0), 0.0001, "paint_white")]
    it.part("body").add(body, neck, *hd, tail, *marks)
    it.part("switch", mech="switch").add(button)
    it.socket("mount", (0, 0, 0))
    it.socket("emitter", em)
    return it
