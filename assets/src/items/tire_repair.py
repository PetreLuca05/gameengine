"""Tire repair kit (data/items.json "tire_repair"): a moulded case (lid open on its hinge, separate) with
its latch and handle, inside: the T-handle reamer and the plug insertion needle (separate parts), a fan of
plug strips, a cement tube and spare plug pack. Palm-held. Procedural (path b)."""
import item_lib as L

W, H, D = 0.2, 0.05, 0.11


def t_tool(x, z, tip, mat_handle):
    # T handle along x, shaft along -z.
    out = [L.cyl((x - 0.03, 0.022, z), (x + 0.03, 0.022, z), 0.0085, 24, mat_handle, bevel=0.0012)]
    out.append(L.cyl((x, 0.022, z - 0.006), (x, 0.022, z - 0.07), 0.0028, 16, "steel_bare", bevel=0.0003))
    if tip == "rasp":
        out.append(L.cyl((x, 0.022, z - 0.07), (x, 0.022, z - 0.095), 0.0033, 16, "steel_bare", bevel=0))
        out[-1].cut(L.merge(*[L.torus((x, 0.022, z - 0.072 - k * 0.002), (0, 0, 1), 0.0033, 0.0005, 16, 4) for k in range(11)]))
        out.append(L.lathe([(0.0033, 0), (0, 0.008)], (x, 0.022, z - 0.095), (0, 0, -1), 16, "steel_bare"))
    else:
        n = L.cyl((x, 0.022, z - 0.07), (x, 0.022, z - 0.1), 0.0026, 16, "steel_bare", bevel=0)
        n.cut(L.box((x, 0.022, z - 0.096), (0.0008, 0.004, 0.005), 0, 0))
        out.append(n)
    return out


def build(ctx):
    it = L.Item("tire_repair", "item")
    case = L.box((0, H * 0.35, 0), (W * 0.5, H * 0.35, D * 0.5), 0.006, 3, "plastic_red")
    case.cut(L.box((0, H * 0.4, 0), (W * 0.5 - 0.004, H * 0.35, D * 0.5 - 0.004), 0.003, 2))
    # The lid built closed on the case (top at H * 0.7 + 0.024), then opened 105 deg about the rear hinge.
    lid = L.box((0, H * 0.7 + 0.012, 0), (W * 0.5, 0.012, D * 0.5), 0.006, 3, "plastic_red")
    lid.cut(L.box((0, H * 0.7 + 0.006, 0), (W * 0.5 - 0.004, 0.012, D * 0.5 - 0.004), 0.003, 2))
    lid_txt = L.text("TIRE REPAIR", (0, H * 0.7 + 0.0242, 0), 0.012, (0, 1, 0), (0, 0, -1), 0.0003, "paint_white")
    for s_ in (lid, lid_txt):
        s_.rotate((1, 0, 0), -105, (0, H * 0.7, D * 0.5))
    foam = L.box((0, H * 0.25, 0), (W * 0.5 - 0.005, H * 0.25, D * 0.5 - 0.005), 0.002, 2, "webbing_black")
    hinge = L.cyl((-W * 0.5 + 0.01, H * 0.7, D * 0.5), (W * 0.5 - 0.01, H * 0.7, D * 0.5), 0.003, 12, "plastic_red", bevel=0.0004)
    latch = L.box((0, H * 0.6, -D * 0.5 - 0.002), (0.012, 0.008, 0.002), 0.001, 2, "plastic_black")
    plugs = [L.box((0.03 + k * 0.007, H * 0.5 + 0.0015, 0.008), (0.0025, 0.0012, 0.04), 0.0006, 1, "rubber").rotate((0, 1, 0), (k - 2) * 6, (0.03, 0, 0.04)) for k in range(5)]
    tube = L.lathe([(0, 0), (0.0085, 0), (0.0085, 0.05), (0.004, 0.056), (0.004, 0.062), (0, 0.062)], (0.07, H * 0.5 + 0.008, 0.03), (0, 0, -1), 24, "aluminium_bare")
    tlab = L.ring_lathe([(0.0086, 0), (0.0088, 0), (0.0088, 0.03), (0.0086, 0.03)], (0.07, H * 0.5 + 0.008, 0.02), (0, 0, -1), 24, "label_yellow")
    it.part("case").add(case, foam, hinge, latch, *plugs, tube, tlab)
    it.part("lid", pivot=(0, H * 0.7, D * 0.5), mech="lid").add(lid, lid_txt)
    it.part("reamer", mech="tool").add(*t_tool(-0.05, 0.045, "rasp", "plastic_yellow"))
    it.part("plug_tool", mech="tool").add(*t_tool(-0.0, 0.045, "needle", "plastic_black"))
    it.socket("grip_r", (0, 0, 0))
    it.socket("lid", (0, H * 0.7, D * 0.5))
    it.hold_small()
    return it
