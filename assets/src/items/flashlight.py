"""Flashlight (data/items.json "flashlight", new): handheld tactical torch: knurled aluminium body, a
crenelated head with reflector / LED / lens, a forward clicky tail cap with a rubber boot, a pocket clip
and a lanyard ring. Held in the fist (origin = grip_r round the body), pointing -Z; the flashlight (F)
shines from its emitter socket while it is in the hands. Procedural (path b)."""
import _light as G
import item_lib as L


def build(ctx):
    it = L.Item("flashlight", "item")
    y = 0.012
    body = G.knurled_tube(0.06, -0.05, y, 0.0128, bands=((0.035, -0.035),), rings=0)
    hd, em = G.head(-0.084, 0.0175, y, depth=0.034)
    tail = L.lathe([(0, 0), (0.0128, 0), (0.0135, 0.004), (0.0135, 0.016), (0.012, 0.018), (0, 0.018)], (0, y, 0.06), (0, 0, 1), 48, "aluminium_anodized")
    boot = L.lathe([(0, 0), (0.0095, 0), (0.0095, 0.004), (0.007, 0.0058), (0, 0.006)], (0, y, 0.078), (0, 0, 1), 32, "rubber")
    clip = L.side_prism([(0.068, 0.0128 + y), (0.02, 0.0128 + y + 0.0018), (0.018, 0.0128 + y + 0.004), (0.066, 0.0128 + y + 0.0036)], -0.004, 0.004, bevel=0.0006, segs=2,
                        mat="steel_parkerized")
    ring = L.torus((0, y - 0.016, 0.07), (1, 0, 0), 0.006, 0.0011, 24, 8, "steel_stainless")
    lanyard = L.torus((0, y - 0.03, 0.074), (1, 0, 0.3), 0.013, 0.0016, 32, 8, "webbing_black")
    marks = [L.text("DEADGROUND  TL-2", (0.0129, y, -0.005), 0.0038, (1, 0, 0), (0, 1, 0), 0.0001, "paint_white")]
    fins = L.cyl((0, y, -0.05), (0, y, -0.062), 0.0158, 48, "aluminium_anodized", bevel=0.0004)
    fins.cut(L.merge(*[L.torus((0, y, -0.052 - k * 0.002), (0, 0, 1), 0.0158, 0.0006, 48, 6) for k in range(5)]))
    oring = L.torus((0, y, 0.0598), (0, 0, 1), 0.0129, 0.0006, 48, 8, "rubber")
    clip_screws = [L.screw((0, 0.0128 + y + 0.0036, z), (0, 1, 0), 0.0011, 0.0004, slot=False, hexsock=True) for z in (0.062, 0.056)]
    it.part("body").add(body, *hd, tail, clip, ring, lanyard, *marks, fins, oring, *clip_screws)
    it.part("tail_switch", mech="switch").add(boot)
    it.socket("grip_r", (0, 0, 0))
    it.socket("emitter", em)
    return it
