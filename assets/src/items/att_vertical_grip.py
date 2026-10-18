"""Vertical grip (assets/attachments/vertical_grip.json): polymer foregrip on an M-LOK / Picatinny clamp:
finger ridges on the front, textured sides, a storage cap at the bottom. Procedural (path b).
grip_l from the JSON (-0.012, -0.05, 0.004)."""
import item_lib as L


def build(ctx):
    it = L.Item("att_vertical_grip", "attachment")
    clamp = L.box((0, -0.005, 0), (0.0128, 0.005, 0.026), 0.0015, 2, "polymer_matte")
    screws = [L.screw((0.0128, -0.005, z), (1, 0, 0), 0.002, 0.0006, slot=False, hexsock=True) for z in (-0.016, 0.016)]
    body = L.side_prism([(-0.014, -0.009), (0.018, -0.009), (0.02, -0.03), (0.019, -0.088), (0.016, -0.096), (-0.008, -0.096), (-0.012, -0.088), (-0.011, -0.06), (-0.015, -0.05),
                         (-0.011, -0.04), (-0.015, -0.03), (-0.012, -0.02)], -0.0128, 0.0128, bevel=0.003, segs=3, mat="polymer_matte", angle=20)
    panel = lambda y, z: -0.088 < y < -0.016 and -0.005 < z < 0.015
    for sx in (1, -1):
        body.cut(L.stipple(sx * 0.0128, -0.09, -0.014, -0.008, 0.017, side=sx, inside=panel))
    capr = L.cyl((0.004, -0.096, 0.004), (0.004, -0.101, 0.004), 0.0115, 40, "polymer_matte", bevel=0.0012)
    capr.cut(L.knurl((0.004, -0.0965, 0.004), (0.004, -0.1008, 0.004), 0.0115, 24, 0.0006))
    it.part("clamp").add(clamp, *screws)
    it.part("grip").add(body)
    it.part("cap", detail=True).add(capr)
    it.socket("mount", (0, 0, 0))
    it.socket("grip_l", (-0.012, -0.05, 0.004))
    return it
