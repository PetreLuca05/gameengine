"""AR-pattern weapon builder (rifle, DMR, SMG): every gameplay dimension comes from the weapon JSON (bore,
rail top = iron rear_base, sight line, front sight z, muzzle, slot positions), the look from a config:
receiver finish, magazine style, stock style, muzzle device, barrel weight, markings. Engine item space."""
import math

import _parts as P
import item_lib as L

DEFAULT = dict(finish="aluminium_anodized", barrel_r=0.0105, mag="stanag", stock="carbine", device="a2", gas=True, forward_assist=True,
               marks=("DEADGROUND ARMS", "DG-15  CAL 5.56 NATO"), serial="SN DG0042177", mag_len=0.0644, mag_drop=0.184, receiver_len=0.0)


def expect(w):
    """The JSON's gameplay sockets (as tools/validate_item derives them)."""
    boxes = w.get("model", [])
    b = min(boxes, key=lambda b: b["offset"][2] - b["half"][2])
    muzzle = (b["offset"][0], b["offset"][1], b["offset"][2] - b["half"][2] - 0.01)
    isg = w["iron_sights"]
    slots = {k: tuple(v["pos"]) for k, v in w.get("attachment_slots", {}).items() if "pos" in v}
    gl = w.get("hold", {}).get("sockets", {}).get("grip_l", {}).get("pos", w.get("support"))
    return dict(muzzle=muzzle, line=isg["line"], rear_z=isg.get("rear_z", 0.0), front_z=isg["front_z"], rail_top=isg.get("rear_base", 0.085), slots=slots,
                grip_l=tuple(gl))


class AR:
    def __init__(self, w, **cfg):
        self.c = dict(DEFAULT)
        self.c.update(cfg)
        e = expect(w)
        self.e = e
        self.bore = e["muzzle"][1]
        self.muzzle_z = e["muzzle"][2]
        self.rail_top = e["rail_top"]
        self.rail_base = self.rail_top - 0.0094
        self.line = e["line"]
        mag = e["slots"]["magazine"]
        self.mag_y, self.magz = mag[1], mag[2]
        hl = self.c["mag_len"] * 0.5
        self.well_f = self.magz - hl - 0.0195
        self.well_r = self.magz + hl + 0.0029
        self.upper_f = self.well_f - 0.010 - self.c["receiver_len"]
        self.upper_r = 0.071
        self.hg_end = e["front_z"] + 0.012
        self.fin = self.c["finish"]

    # ---------------------------------------------------------------- receivers
    def upper(self):
        hw = 0.0142
        b, rb, f, r = self.bore - 0.016, self.rail_base, self.upper_f, self.upper_r
        pts = [(f, b), (r, b), (r, rb - 0.0136), (r + 0.005, rb - 0.0096), (r + 0.005, rb), (f, rb)]
        up = L.side_prism(pts, -hw, hw, bevel=0.0012, segs=2, mat=self.fin)
        port_z = self.magz + 0.044
        up.cut(L.box((hw, self.bore + 0.002, port_z), (0.006, 0.0075, 0.025), 0.0, 0))
        up.cut(L.cyl((0, self.bore + 0.002, f - 0.01), (0, self.bore + 0.002, r + 0.01), 0.0108, 32, self.fin, bevel=0))
        out = [up]
        defl = L.side_prism([(port_z + 0.032, b + 0.01), (port_z + 0.052, b + 0.01), (port_z + 0.052, b + 0.036), (port_z + 0.042, b + 0.036)], hw - 0.002,
                            hw + 0.006, bevel=0.0015, segs=2, mat=self.fin)
        out.append(defl)
        if self.c["forward_assist"]:
            out.append(L.cyl((hw + 0.001, rb - 0.0146, port_z + 0.058), (hw + 0.009, rb - 0.0146, port_z + 0.094), 0.0075, 28, self.fin, bevel=0.0008))
        out.append(L.picatinny(f, r + 0.001, rb, mat=self.fin))
        for k in range(0, int((r - f) / 0.01001) - 1, 2):
            z = f + 0.007 + k * 0.01001
            out.append(L.text("T%d" % (k + 1), (-0.0107, rb + 0.0035, z), 0.0026, normal=(-1, 0, 0), up=(0, 1, 0), depth=0.00015, mat="engrave"))
        return out

    def lower(self):
        hw = 0.0118
        f, r, top = self.well_f, self.well_r, self.bore - 0.016
        bot = -0.03
        pts = [(f, top), (f, 0.012), (f + 0.01, -0.006), (f + 0.014, bot), (r, bot), (r + 0.002, -0.004), (-0.012, -0.004), (-0.004, 0.006), (0.032, 0.006),
               (0.05, 0.012), (self.upper_r, 0.02), (self.upper_r, top)]
        low = L.side_prism(pts, -hw, hw, bevel=0.0012, segs=2, mat=self.fin)
        flare = L.side_prism([(f + 0.008, -0.008), (r + 0.003, -0.008), (r + 0.0045, bot - 0.001), (f + 0.0105, bot - 0.001)], -0.0158, 0.0158, bevel=0.0018,
                             segs=2, mat=self.fin)
        hl = self.c["mag_len"] * 0.5
        flare.cut(L.box((0, -0.02, self.magz - 0.0035), (0.0118, 0.02, hl + 0.0003), 0, 0))
        low.cut(L.box((0, -0.005, self.magz - 0.0035), (0.0113, 0.03, hl + 0.0003), 0, 0))
        tg = L.side_prism([(r, -0.006), (r + 0.002, -0.02), (-0.03, -0.0255), (-0.006, -0.0215), (0.004, -0.008), (0.0, -0.005), (-0.008, -0.016),
                           (-0.03, -0.0205), (r + 0.006, -0.016), (r + 0.004, -0.006)], -0.0055, 0.0055, bevel=0.0012, segs=2, mat=self.fin, angle=20)
        low.cut(L.box((0, -0.002, -0.026), (0.003, 0.008, 0.0125), 0, 0))
        pin_f = (f + 0.007, top - 0.002)
        pin_r = (0.062, top - 0.004)
        bosses = []
        for (z, y, rr) in ((pin_f[0], pin_f[1], 0.0042), (pin_r[0], pin_r[1], 0.0038)):
            for sx in (1, -1):
                bosses.append(L.cyl((sx * (hw - 0.0005), y, z), (sx * (hw + 0.002), y, z), rr, 20, self.fin, bevel=0.0005))
        fence = L.ring_lathe([(0.0045, 0), (0.0062, 0), (0.0062, 0.0022), (0.0045, 0.0022)], (hw, 0.012, r - 0.0015), (1, 0, 0), 24, self.fin, bevel=0.0004)
        marks = [L.text(self.c["marks"][0], (-0.0158, -0.012, self.magz - 0.0035), 0.0042, (-1, 0, 0), (0, 1, 0), 0.00018),
                 L.text(self.c["marks"][1], (-0.0158, -0.0185, self.magz - 0.0035), 0.0032, (-1, 0, 0), (0, 1, 0), 0.00018),
                 L.text("SAFE", (-hw, 0.031, -0.004), 0.0022, (-1, 0, 0), (0, 1, 0), 0.00015),
                 L.text("SEMI", (-hw, 0.0125, 0.0115), 0.0022, (-1, 0, 0), (0, 1, 0), 0.00015),
                 L.text("AUTO", (-hw, 0.0095, 0.0005), 0.0022, (-1, 0, 0), (0, 1, 0), 0.00015)]
        self.pins = (pin_f, pin_r)
        return [low, flare, tg] + bosses + [fence] + marks

    def serial_plate(self):
        hw, z = 0.0158, self.magz - 0.0035
        plate = L.box((-hw - 0.0004, -0.024, z), (0.0004, 0.0035, 0.018), 0.0003, 1, "steel_stainless")
        num = L.text(self.c["serial"], (-hw - 0.0008, -0.024, z), 0.0028, (-1, 0, 0), (0, 1, 0), 0.00015, mat="paint_black")
        rivets = [L.cyl((-hw - 0.0008, -0.024, zz), (-hw - 0.0012, -0.024, zz), 0.0008, 10, "steel_stainless", bevel=0.0001) for zz in (z - 0.016, z + 0.016)]
        return [plate, num] + rivets

    # ---------------------------------------------------------------- front end
    def handguard(self):
        z0, z1 = self.upper_f, self.hg_end
        w, top, bot = 0.0205, self.rail_base, self.bore - 0.0335
        c = 0.0075
        sec = [(-w + c, bot), (w - c, bot), (w, bot + c), (w, top - c * 0.6), (w - c * 0.6, top), (-w + c * 0.6, top), (-w, top - c * 0.6), (-w, bot + c)]
        hg = L.front_prism(sec, z1, z0, bevel=0.0012, segs=2, mat=self.fin)
        hg.cut(L.cyl((0, self.bore, z1 - 0.01), (0, self.bore, z0 + 0.01), 0.0155, 32, self.fin, bevel=0))
        cuts = [L.mlok_slots(z1 + 0.025, z0 - 0.03, (w, self.bore - 0.004, 0), (1, 0, 0)), L.mlok_slots(z1 + 0.025, z0 - 0.03, (-w, self.bore - 0.004, 0), (-1, 0, 0)),
                L.mlok_slots(z1 + 0.025, z0 - 0.03, (0, bot, 0), (0, -1, 0)),
                L.mlok_slots(z1 + 0.045, z0 - 0.05, (w - c * 0.5, bot + c * 0.5, 0), (0.7071, -0.7071, 0)),
                L.mlok_slots(z1 + 0.045, z0 - 0.05, (-w + c * 0.5, bot + c * 0.5, 0), (-0.7071, -0.7071, 0))]
        n = int((z0 - z1 - 0.04) / 0.045)
        for k in range(n):
            z = z1 + 0.03 + k * 0.045
            for sx in (1, -1):
                cuts.append(L.cyl((sx * (w + 0.003), top - 0.012, z), (sx * (w - 0.005), top - 0.012, z), 0.0035, 16, self.fin, bevel=0))
        hg.cut(*[x for x in cuts if x is not None])
        rail = L.picatinny(z1 - 0.027, z0, self.rail_base, mat=self.fin)
        ext = L.box((0, self.rail_base - 0.006, z1 - 0.0135), (0.0105, 0.006, 0.0135), 0.0008, 2, self.fin)
        screws = [L.screw((sx * w, self.bore - 0.016, z), (sx, 0, 0), 0.0021, 0.0007, slot=False, hexsock=True) for z in (z0 - 0.012, z0 - 0.026) for sx in (1, -1)]
        qd = L.tube((-w - 0.0005, self.bore - 0.01, z1 + 0.012), (-w - 0.007, self.bore - 0.01, z1 + 0.012), 0.0058, 0.0039, 24, "steel_parkerized", bevel=0.0004)
        ring = L.ring_lathe([(0.0128, 0), (0.0185, 0), (0.0185, 0.003), (0.0128, 0.003)], (0, self.bore - 0.003, z1 + 0.0005), (0, 0, -1), 32, self.fin, bevel=0.0006)
        return [hg, rail, ext, qd, ring] + screws

    def barrel(self):
        z0 = self.upper_f
        L_ = z0 - self.muzzle_z
        r = self.c["barrel_r"]
        dev = self.c["device"]
        thread_len = 0.013 if dev != "none" else 0.0
        hg = z0 - self.hg_end
        prof = [(0.0, 0.0), (0.0128, 0.0), (0.0128, 0.012), (r, 0.016), (r, hg - 0.004), (r - 0.0011, hg + 0.002), (r - 0.0011, L_ - thread_len - 0.04),
                (r - 0.0023, L_ - thread_len - 0.037), (r - 0.0023, L_ - thread_len - 0.002), (0.0072, L_ - thread_len), (0.0072, L_), (0.0, L_)]
        b = L.lathe(prof, (0, self.bore, z0), (0, 0, -1), 40, "steel_parkerized")
        b.cut(L.cyl((0, self.bore, self.muzzle_z - 0.003), (0, self.bore, self.muzzle_z + 0.012), 0.0029 if r > 0.0098 else 0.0045, 20, bevel=0))
        out = [b]
        if thread_len:
            out += [L.torus((0, self.bore, self.muzzle_z + 0.0005 + k * 0.0009), (0, 0, -1), 0.0072, 0.00035, 32, 6, "steel_parkerized") for k in range(13)]
        if self.c["gas"]:
            gz = self.hg_end + 0.028
            gb = L.box((0, self.bore - 0.002, gz), (0.0115, 0.0125, 0.011), 0.0015, 2, "steel_parkerized")
            gb.cut(L.cyl((0, self.bore, gz - 0.015), (0, self.bore, gz + 0.015), r - 0.0011, 32, bevel=0))
            out += [gb, L.cyl((0, self.bore + 0.0145, gz + 0.008), (0, self.bore + 0.0145, z0 + 0.012), 0.0024, 12, "steel_stainless", bevel=0.0003)]
            out += [L.pin((0, self.bore - 0.008, z), (1, 0, 0), 0.0012, 0.025) for z in (gz - 0.005, gz + 0.005)]
        return out

    def muzzle_device(self):
        z, by, dev = self.muzzle_z, self.bore, self.c["device"]
        if dev == "a2":
            prof = [(0.0076, 0.0), (0.0108, 0.0), (0.0112, 0.002), (0.0112, 0.0435), (0.0104, 0.046), (0.0068, 0.046), (0.0068, 0.03), (0.0076, 0.028)]
            fh = L.ring_lathe(prof, (0, by, z + 0.046), (0, 0, -1), 40, "steel_parkerized", bevel=0.0004)
            cuts = []
            for ang in (90, 30, 150, -30, 210):
                a = math.radians(ang)
                c = L.box((0, 0, 0), (0.0018, 0.012, 0.0115), 0, 0)
                c.rotate((0, 0, 1), ang - 90)
                c.move((math.cos(a) * 0.009, by + math.sin(a) * 0.009, z + 0.018))
                cuts.append(c)
            fh.cut(L.merge(*cuts))
            washer = L.ring_lathe([(0.0073, 0), (0.0112, 0), (0.0112, 0.0018), (0.0073, 0.0018)], (0, by, z + 0.0478), (0, 0, -1), 32, "steel_dark")
            return [fh, washer]
        if dev == "brake":
            # Two-chamber brake: side ports, top ports (the integral comp of the DMR).
            prof = [(0.0076, 0.0), (0.0122, 0.0), (0.0128, 0.003), (0.0128, 0.052), (0.0118, 0.055), (0.0058, 0.055), (0.0058, 0.035), (0.0076, 0.033)]
            br = L.ring_lathe(prof, (0, by, z + 0.055), (0, 0, -1), 48, "steel_parkerized", bevel=0.0004)
            cuts = []
            for zz in (z + 0.012, z + 0.028):
                cuts.append(L.box((0, by, zz), (0.02, 0.0062, 0.0055), 0, 0))
            for k in range(3):
                cuts.append(L.cyl((0.004 * (k - 1), by + 0.016, z + 0.042), (0.004 * (k - 1), by + 0.004, z + 0.042), 0.0012, 10, bevel=0))
            br.cut(L.merge(*cuts))
            nut = L.ring_lathe([(0.0073, 0), (0.0122, 0), (0.0122, 0.004), (0.0073, 0.004)], (0, by, z + 0.059), (0, 0, -1), 6, "steel_dark")
            return [br, nut]
        if dev == "thread":
            prof = [(0.0068, 0.0), (0.0085, 0.0), (0.0089, 0.0015), (0.0089, 0.0125), (0.0068, 0.0125)]
            tp = L.ring_lathe(prof, (0, by, z + 0.0125), (0, 0, -1), 40, "steel_parkerized", bevel=0.0003)
            tp.cut(L.knurl((0, by, z + 0.011), (0, by, z + 0.002), 0.0089, 24, 0.0004))
            return [tp]
        return []

    # ---------------------------------------------------------------- moving parts
    def bolt_carrier(self):
        y, z0 = self.bore + 0.002, self.magz + 0.01
        bc = L.cyl((0, y, z0), (0, y, z0 + 0.14), 0.0102, 32, "steel_dark", bevel=0.0008)
        bc.cut(L.box((0.0102, y, self.magz + 0.06), (0.002, 0.0055, 0.022), 0, 0))
        bolt = L.cyl((0, y, z0 - 0.017), (0, y, z0), 0.0068, 24, "steel_dark", bevel=0.0005)
        lugs = [L.box((math.cos(a) * 0.0068, y + math.sin(a) * 0.0068, z0 - 0.014), (0.0014, 0.0014, 0.003), 0.0003, 1, "steel_dark") for a in
                [k * math.pi / 3.5 for k in range(7)]]
        cam = L.cyl((0.003, y + 0.01, z0 + 0.035), (0.003, y + 0.013, z0 + 0.035), 0.0022, 12, "steel_dark", bevel=0.0003)
        extractor = L.box((0.0066, y, z0 - 0.01), (0.0012, 0.002, 0.008), 0.0003, 1, "steel_dark")
        return [bc, bolt, cam, extractor] + lugs

    def charging_handle(self):
        y0, y1 = self.rail_base - 0.0101, self.rail_base - 0.0021
        z = self.upper_r
        ch = L.top_prism([(-0.0045, z - 0.005), (0.0045, z - 0.005), (0.0045, z + 0.011), (0.02, z + 0.012), (0.022, z + 0.019), (0.018, z + 0.023),
                          (-0.018, z + 0.023), (-0.022, z + 0.019), (-0.02, z + 0.012), (-0.0045, z + 0.011)], y0, y1, bevel=0.0012, segs=2, mat=self.fin,
                         angle=20)
        latch = L.top_prism([(0.009, z + 0.015), (0.019, z + 0.014), (0.026, z + 0.02), (0.024, z + 0.024), (0.016, z + 0.023)], y0 + 0.001, y1 - 0.001,
                            bevel=0.0008, segs=2, mat=self.fin)
        return [ch, latch]

    def dust_cover(self, piv):
        dc = L.box((0.0, -0.0006, 0.0), (0.0006, 0.0072, 0.0245), 0.0003, 1, "steel_parkerized")
        dc.rotate((0, 0, 1), -100)
        hinge = L.cyl((0, 0, 0.026), (0, 0, -0.026), 0.0012, 12, "steel_dark", bevel=0.0002)
        return [s.move(piv) for s in (dc, hinge)]

    def safety(self, piv):
        lv = L.top_prism([(-0.0028, -0.002), (0.0028, -0.002), (0.0022, 0.016), (-0.0022, 0.016)], -0.0015, 0.0015, bevel=0.0006, segs=2, mat="steel_parkerized")
        lv.rotate((1, 0, 0), 90)
        lv.rotate((0, 0, 1), 90)
        hub = L.cyl((0, 0, 0), (-0.0022, 0, 0), 0.0045, 20, "steel_parkerized", bevel=0.0005)
        return [s.move(piv) for s in (lv.move((-0.0035, 0, 0)), hub)]

    def magazine(self):
        top = self.mag_y + 0.004
        hl = self.c["mag_len"] * 0.5
        zf, zb = self.magz - 0.0035 - hl + 0.0003, self.magz - 0.0035 + hl - 0.0003
        st = self.c["mag"]
        if st == "stanag":
            return P.stanag_mag(top, top - self.c["mag_drop"], zf, zb)
        if st == "762":
            return P.stanag_mag(top, top - self.c["mag_drop"], zf, zb, half_w=0.0118, curve=0.008, mat="polymer_matte", windows=False)
        return P.stanag_mag(top, top - self.c["mag_drop"], zf, zb, half_w=0.0105, curve=0.004, mat="polymer_matte", windows=True)

    # ---------------------------------------------------------------- stocks
    def stock(self, it, tube_y):
        st = self.c["stock"]
        if st == "carbine":
            tube, nut, plate, pos_z = P.buffer_tube(self.upper_r + 0.004, self.upper_r + 0.191, tube_y)
            t = it.part("stock_tube", hide="stock").add(tube, nut, plate)
            it.part("stock", parent=t, pivot=(0, tube_y, 0.2), hide="stock").add(*carbine_stock(tube_y, self.fin))
            return pos_z
        if st == "precision":
            tube, nut, plate, pos_z = P.buffer_tube(self.upper_r + 0.004, self.upper_r + 0.19, tube_y, positions=0)
            t = it.part("stock_tube", hide="stock").add(tube, nut, plate)
            it.part("stock", parent=t, pivot=(0, tube_y, 0.22), hide="stock").add(*precision_stock(tube_y))
            return []
        # PDW: two telescoping rods, a butt plate and a cheek bar.
        t = it.part("stock", hide="stock")
        z0, z1 = self.upper_r + 0.004, self.upper_r + 0.15
        for sx in (1, -1):
            t.add(L.cyl((sx * 0.0085, tube_y + 0.004, z0), (sx * 0.0085, tube_y + 0.004, z1), 0.0045, 20, "steel_parkerized", bevel=0.0005))
        t.add(L.box((0, tube_y + 0.004, z0 + 0.008), (0.0145, 0.009, 0.008), 0.0015, 2, self.fin))
        bp = L.side_prism([(z1 - 0.006, tube_y + 0.03), (z1 + 0.006, tube_y + 0.03), (z1 + 0.008, tube_y - 0.05), (z1 - 0.004, tube_y - 0.05)], -0.017, 0.017,
                          bevel=0.003, segs=3, mat="polymer_matte")
        pad = L.side_prism([(z1 + 0.006, tube_y + 0.03), (z1 + 0.014, tube_y + 0.029), (z1 + 0.015, tube_y - 0.049), (z1 + 0.008, tube_y - 0.05)], -0.0175,
                           0.0175, bevel=0.0025, segs=3, mat="rubber")
        pad.cut(L.merge(*[L.box((0, tube_y + 0.022 - k * 0.0085, z1 + 0.015), (0.02, 0.0012, 0.0012), 0, 0) for k in range(9)]))
        t.add(bp, pad, L.box((0, tube_y + 0.032, (z0 + z1) * 0.5 + 0.03), (0.007, 0.004, 0.05), 0.0018, 2, "polymer_matte"))
        return []

    # ---------------------------------------------------------------- assembly
    def build(self, item_id):
        it = L.Item(item_id, "weapon")
        e = self.e
        it.part("upper_receiver").add(*self.upper())
        it.part("lower_receiver").add(*self.lower())
        it.part("serial_plate", detail=True).add(*self.serial_plate())
        g, cap = P.ar_pistol_grip()
        it.part("pistol_grip").add(g, cap, L.screw((0, -0.073, 0.027), (0, -1, 0), 0.0025, 0.0008, hexsock=True, slot=False))
        it.part("handguard").add(*self.handguard())
        it.part("barrel").add(*self.barrel())
        dev = self.muzzle_device()
        if dev:
            it.part("muzzle_device", hide="muzzle").add(*dev)
        it.part("bolt_carrier", pivot=(0, self.bore, self.magz + 0.06), mech="bolt").add(*self.bolt_carrier())
        it.part("charging_handle", pivot=(0, self.rail_base - 0.006, self.upper_r + 0.009), mech="charging_handle").add(*self.charging_handle())
        piv = (0.0145, self.bore - 0.008, self.magz + 0.044)
        it.part("dust_cover", pivot=piv, mech="dust_cover").add(*self.dust_cover(piv))
        it.part("trigger", pivot=(0, 0.012, -0.027), mech="trigger").add(P.ar_trigger((0, 0.012, -0.027)))
        it.part("hammer_pins", detail=True).add(L.pin((0, 0.012, -0.027), (1, 0, 0), 0.0015, 0.0246), L.pin((0, 0.014, -0.006), (1, 0, 0), 0.0015, 0.0246))
        sp = (-0.0118, 0.019, 0.006)
        it.part("safety", pivot=sp, mech="safety").add(*self.safety(sp))
        r = self.well_r
        mr = L.cyl((0.0118, 0.012, r - 0.0015), (0.0158, 0.012, r - 0.0015), 0.0042, 24, "steel_parkerized", bevel=0.0006)
        mr.cut(L.serrations(r - 0.006, r + 0.003, 0.009, 0.015, 0.0158, 0.0011, 0.0003))
        it.part("mag_release", pivot=(0.0118, 0.012, r - 0.0015), mech="mag_release").add(mr)
        bz = r + 0.007
        bc = L.side_prism([(bz - 0.006, 0.02), (bz + 0.008, 0.02), (bz + 0.01, 0.034), (bz + 0.004, 0.038), (bz - 0.008, 0.03)], -0.0145, -0.0118, bevel=0.0006,
                          segs=2, mat="steel_parkerized")
        bc.cut(L.serrations(bz - 0.006, bz + 0.007, 0.031, 0.037, -0.0145, 0.0012, 0.0003))
        it.part("bolt_catch", pivot=(-0.013, 0.025, bz), mech="bolt_catch").add(bc)
        (fz, fy), (rz, ry) = self.pins
        it.part("takedown_pins", detail=True).add(L.pin((0, fy, fz), (1, 0, 0), 0.0024, 0.0322, "steel_parkerized"),
                                                 L.pin((0, ry, rz), (1, 0, 0), 0.0024, 0.0306, "steel_parkerized"))
        if self.c["forward_assist"]:
            pz = self.magz + 0.044
            y = self.rail_base - 0.0146
            fa = L.cyl((0.0222, y, pz + 0.088), (0.0252, y, pz + 0.098), 0.006, 24, "steel_parkerized", bevel=0.0006)
            fa.cut(L.knurl((0.0227, y, pz + 0.09), (0.0252, y, pz + 0.098), 0.006, 16, 0.0005))
            it.part("forward_assist", detail=True).add(fa)
        body, plate, follower, case, bullet = self.magazine()
        mz = (0, self.mag_y, self.magz)
        mag = it.part("magazine", pivot=mz, mech="mag", hide="magazine").add(body, plate)
        it.part("mag_follower", parent=mag, pivot=(0, self.mag_y - 0.002, self.magz)).add(follower)
        it.part("top_round", parent=mag, pivot=(0, self.mag_y + 0.004, self.magz), detail=True).add(case, bullet)
        tube_y = e["slots"]["stock"][1]
        pos_z = self.stock(it, tube_y)
        rb, rleaf, rh = P.flip_rear_sight(e["rear_z"], self.rail_top, self.line)
        rs = it.part("rear_sight", hide="optic").add(*rb)
        it.part("rear_sight_leaf", parent=rs, pivot=tuple(rh)).add(*rleaf)
        fb, fleaf, fhinge = P.flip_front_sight(e["front_z"], self.rail_top, self.line)
        it.part("front_sight_base").add(*fb)
        it.part("front_sight", pivot=tuple(fhinge), fold="optic", fold_axis=(1, 0, 0), fold_angle=88.0).add(*fleaf)
        it.socket("grip_r", (0, 0, 0))
        it.socket("grip_l", e["grip_l"])
        it.socket("sight", (0, self.line, e["rear_z"]))
        it.socket("muzzle", e["muzzle"])
        for k in ("optic", "underbarrel", "magazine", "stock"):
            if k in e["slots"]:
                it.socket(k, e["slots"][k])
        pz = self.magz + 0.044
        it.socket("ejection_port", (0.0142, self.bore + 0.002, pz), (0, 90, 0))
        it.socket("shell_eject", (0.02, self.bore + 0.003, pz + 0.006), (0, 70, 0))
        it.socket("charging_handle", (0, self.rail_base - 0.006, self.upper_r + 0.019))
        for k, z in enumerate(pos_z):
            it.socket("stock_pos_%d" % k, (0, tube_y - 0.0147, z))
        return it


def carbine_stock(tube_y, fin):
    z0, z1 = 0.148, 0.308
    pts = [(z0, tube_y + 0.022), (z0 + 0.03, tube_y + 0.027), (z1 - 0.02, tube_y + 0.03), (z1, tube_y + 0.036), (z1, tube_y - 0.068), (z1 - 0.012, tube_y - 0.068),
           (z1 - 0.04, tube_y - 0.03), (z0 + 0.03, tube_y - 0.024), (z0, tube_y - 0.022)]
    body = L.side_prism(pts, -0.0185, 0.0185, bevel=0.003, segs=3, mat="polymer_matte", angle=20)
    body.cut(L.cyl((0, tube_y, z0 - 0.01), (0, tube_y, z1 - 0.025), 0.0152, 32, "polymer_matte", bevel=0))
    body.cut(L.side_prism([(z1 - 0.06, tube_y - 0.022), (z1 - 0.022, tube_y - 0.022), (z1 - 0.018, tube_y - 0.052), (z1 - 0.04, tube_y - 0.036)], -0.03, 0.03, 0.0, 0))
    body.cut(L.box((0, tube_y - 0.057, z1 - 0.012), (0.03, 0.0035, 0.0045), 0, 0))
    pad = L.side_prism([(z1, tube_y + 0.037), (z1 + 0.012, tube_y + 0.035), (z1 + 0.013, tube_y - 0.066), (z1, tube_y - 0.069)], -0.0195, 0.0195, bevel=0.003, segs=3,
                       mat="rubber", angle=20)
    pad.cut(L.merge(*[L.box((0, tube_y + 0.02 - k * 0.0085, z1 + 0.0128), (0.021, 0.0012, 0.0012), 0, 0) for k in range(11)]))
    lever = L.side_prism([(z0 + 0.006, tube_y - 0.022), (z0 + 0.05, tube_y - 0.027), (z0 + 0.052, tube_y - 0.032), (z0 + 0.008, tube_y - 0.029)], -0.006, 0.006,
                         bevel=0.0012, segs=2, mat="polymer_matte")
    pin = L.pin((0, tube_y - 0.025, z0 + 0.044), (1, 0, 0), 0.0016, 0.02)
    return [body, pad, lever, pin]


def precision_stock(tube_y):
    """Fixed precision stock: long body, adjustable cheek riser (two posts), monopod rail, QD cups."""
    z0, z1 = 0.11, 0.34
    pts = [(z0, tube_y + 0.02), (z0 + 0.05, tube_y + 0.024), (z1 - 0.01, tube_y + 0.024), (z1, tube_y + 0.034), (z1, tube_y - 0.074), (z1 - 0.03, tube_y - 0.074),
           (z1 - 0.07, tube_y - 0.05), (z0 + 0.09, tube_y - 0.046), (z0 + 0.03, tube_y - 0.026), (z0, tube_y - 0.022)]
    body = L.side_prism(pts, -0.019, 0.019, bevel=0.0035, segs=3, mat="polymer_tan", angle=20)
    body.cut(L.cyl((0, tube_y, z0 - 0.01), (0, tube_y, z0 + 0.17), 0.0152, 32, bevel=0))
    body.cut(L.side_prism([(z0 + 0.1, tube_y - 0.02), (z1 - 0.06, tube_y - 0.02), (z1 - 0.07, tube_y - 0.042), (z0 + 0.11, tube_y - 0.038)], -0.03, 0.03, 0.0, 0))
    cheek = L.side_prism([(z0 + 0.07, tube_y + 0.044), (z1 - 0.025, tube_y + 0.044), (z1 - 0.02, tube_y + 0.036), (z0 + 0.075, tube_y + 0.034)], -0.0175, 0.0175,
                         bevel=0.003, segs=3, mat="polymer_tan")
    posts = [L.cyl((0, tube_y + 0.023, z), (0, tube_y + 0.035, z), 0.0035, 16, "steel_dark", bevel=0.0004) for z in (z0 + 0.1, z1 - 0.06)]
    knob = L.cyl((0.019, tube_y + 0.03, z0 + 0.1), (0.026, tube_y + 0.03, z0 + 0.1), 0.006, 20, "aluminium_anodized", bevel=0.0006)
    knob.cut(L.knurl((0.0195, tube_y + 0.03, z0 + 0.1), (0.026, tube_y + 0.03, z0 + 0.1), 0.006, 18, 0.0004))
    pad = L.side_prism([(z1, tube_y + 0.035), (z1 + 0.014, tube_y + 0.033), (z1 + 0.015, tube_y - 0.072), (z1, tube_y - 0.075)], -0.0198, 0.0198, bevel=0.003, segs=3,
                       mat="rubber")
    pad.cut(L.merge(*[L.box((0, tube_y + 0.025 - k * 0.009, z1 + 0.015), (0.021, 0.0012, 0.0012), 0, 0) for k in range(11)]))
    rail = L.picatinny(z1 - 0.075, z1 - 0.025, tube_y - 0.074, mat="aluminium_anodized")
    rail.rotate((0, 0, 1), 180, (0, tube_y - 0.074, 0))
    qds = [L.tube((sx * 0.0185, tube_y - 0.03, z1 - 0.05), (sx * 0.0225, tube_y - 0.03, z1 - 0.05), 0.0058, 0.0039, 20, "steel_parkerized") for sx in (1, -1)]
    return [body, cheek, knob, pad, rail] + posts + qds
