"""Vellmouth, the city outside unit 10A (docs/CHAPTER_01.md §5, docs/STORY.md "Vista"), from
assets/story/vista/vellmouth.json.

Layers (distances from the view point, the flat's centre at eye height):
  near     real kit facade buildings in the window's view (a batched prefab: one mesh per material set),
           facade boxes elsewhere; sidewalks, the river, its embankments and bridges, Linden Park
  mid      "facade" boxes (procedural window grid, lit at night by district power) out to 1.25 km
  skyline  a ring of low boxes at 1.3-1.42 km that hides the ground's edge
  vista    the landmarks beyond 1.1 km compressed in towards the eye (same angular size, bases extended to
           the ground, colours fogged for the distance they lost), so everything sits inside zfar 1500
Also: the "city" block (window lights; roads for traffic), story points for the vista sequences (smoke,
helicopters, sirens, birds).

Library: build_story01.py calls generate(); `python tools/story/vista_gen.py` prints a summary.
"""
import json, math, os, random

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "assets", "story", "vista", "vellmouth.json")
NEAR_PREFAB = "vista_vellmouth_near"


def r2(v):
    return round(v + 0.0, 2)


class City:
    def __init__(self, cfg):
        self.cfg = cfg
        self.rng = random.Random(cfg["seed"])
        self.vx, self.vy, self.vz = cfg["view"]
        self.brushes, self.kit, self.points, self.roads = [], [], {}, []
        self.stats = {"kit_buildings": 0, "boxes": 0, "landmarks": 0}
        self.exclude = []   # (x, z, radius): landmark sites the blocks keep clear of
        rv = cfg["river"]
        self.river = []   # centre line (x, z, bearing)
        b = rv["from"]
        while b <= rv["to"] + 1e-6:
            x, z = self.at(b, self.river_r(b))
            self.river.append((x, z, b))
            b += 0.5

    # ---------------------------------------------------------------- geometry helpers
    def at(self, bearing, dist):
        a = math.radians(bearing)
        return self.vx + math.sin(a) * dist, self.vz - math.cos(a) * dist

    def polar(self, x, z):
        dx, dz = x - self.vx, z - self.vz
        return math.degrees(math.atan2(dx, -dz)), math.hypot(dx, dz)

    def river_r(self, b):
        rv = self.cfg["river"]
        return rv["base"] + rv["quad"] * b * b + rv["bend"] * max(0.0, b - rv["bend_from"]) ** 2

    def river_dist(self, x, z):
        return min(math.hypot(x - px, z - pz) for px, pz, _ in self.river)

    def box(self, cx, y0, cz, sx, sy, sz, color, material="concrete", yaw=0.0, **kw):
        b = {"type": "box", "pos": [r2(cx), r2(y0), r2(cz)], "size": [r2(sx), r2(sy), r2(sz)], "color": [round(c, 3) for c in color],
             "material": material, "grid": False, "collide": False}
        if yaw:
            b["yaw"] = round(yaw, 1)
        b.update(kw)
        self.brushes.append(b)
        self.stats["boxes"] += 1
        return b

    def style_at(self, bearing):
        for d in self.cfg["districts"]:
            if d["from"] <= bearing < d["to"]:
                return self.cfg["styles"][d["style"]]
        return self.cfg["styles"]["residential"]

    # ---------------------------------------------------------------- ground: river, bridges, park
    def water(self):
        rv = self.cfg["river"]
        w, emb = rv["width"], rv["embankment"]
        pts = self.river
        for i in range(len(pts) - 1):
            (x0, z0, _), (x1, z1, _) = pts[i], pts[i + 1]
            if self.polar(x0, z0)[1] > self.cfg["bands"]["skyline"][1]:
                continue
            L = math.hypot(x1 - x0, z1 - z0)
            yaw = -math.degrees(math.atan2(x1 - x0, -(z1 - z0)))   # local -Z along the segment
            cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
            self.box(cx, -0.6, cz, w, 0.62, L + 0.6, [0.2, 0.27, 0.3], "water", yaw, roughness=0.12, metallic=0.0)
            nx, nz = (z1 - z0) / L, -(x1 - x0) / L   # perpendicular
            for s in (-1, 1):
                ex, ez = cx + nx * s * (w / 2 + emb / 2), cz + nz * s * (w / 2 + emb / 2)
                self.box(ex, 0.0, ez, emb, 0.4, L + 0.6, [0.5, 0.48, 0.44], "concrete", yaw)
                ex, ez = cx + nx * s * (w / 2 + 0.3), cz + nz * s * (w / 2 + 0.3)
                self.box(ex, -0.6, ez, 0.6, 1.4, L + 0.6, [0.42, 0.4, 0.37], "concrete", yaw)

    def bridges(self):
        rv = self.cfg["river"]
        for br in self.cfg["bridges"]:
            b = br["bearing"]
            r = self.river_r(b)
            # The river's tangent here; the bridge crosses it at right angles.
            x0, z0 = self.at(b - 0.5, self.river_r(b - 0.5))
            x1, z1 = self.at(b + 0.5, self.river_r(b + 0.5))
            tyaw = -math.degrees(math.atan2(x1 - x0, -(z1 - z0)))
            yaw = tyaw + 90.0
            cx, cz = self.at(b, r)
            span = rv["width"] + 2 * rv["embankment"] + 50
            deck, wid = br["deck"], br["width"]
            col = br["color"]
            self.box(cx, deck - 1.2, cz, wid, 1.2, span, col, "concrete", yaw)
            self.box(cx, deck, cz, wid - 4, 0.05, span, [0.24, 0.24, 0.25], "asphalt", yaw)
            ux, uz = -math.sin(math.radians(yaw)), -math.cos(math.radians(yaw))   # local -Z (along the deck)
            rx, rz = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))    # local +X (across)
            for s in (-1, 1):   # parapets
                self.box(cx + rx * s * (wid / 2 - 0.3), deck, cz + rz * s * (wid / 2 - 0.3), 0.4, 1.1, span, col, "concrete", yaw)
            n = br["piers"]
            for k in range(n):
                t = (k + 1) / (n + 1) - 0.5
                px, pz = cx + ux * t * rv["width"], cz + uz * t * rv["width"]
                self.box(px, -0.6, pz, wid * 0.8, deck - 0.6, 4.0, col, "concrete", yaw)
            for s in (-1, 1):   # ramps down to the street
                ax, az = cx + ux * s * (span / 2 + 25), cz + uz * s * (span / 2 + 25)
                self.box(ax, 0, az, wid, deck * 0.5, 50, col, "concrete", yaw)
            if br.get("truss"):
                for s in (-1, 1):
                    for k in range(9):
                        t = (k - 4) / 9.0
                        px, pz = cx + ux * t * span * 0.8 + rx * s * (wid / 2), cz + uz * t * span * 0.8 + rz * s * (wid / 2)
                        h = 9.0 * (1 - (t * 2.2) ** 2)
                        if h > 1:
                            self.box(px, deck, pz, 0.6, h, 0.6, [0.3, 0.22, 0.18], "metal_thick", yaw)
                    tx, tz = cx + rx * s * (wid / 2), cz + rz * s * (wid / 2)
                    self.box(tx, deck + 8.0, tz, 0.8, 0.8, span * 0.55, [0.3, 0.22, 0.18], "metal_thick", yaw)
            ends = [(cx + ux * s * (span / 2 + 50), cz + uz * s * (span / 2 + 50)) for s in (1, -1)]
            self.roads.append({"name": br["name"], "points": [[r2(x), deck if i == 1 else 0.0, r2(z)] for i, (x, z) in
                                                              enumerate([ends[0], (cx, cz), ends[1]])]})
            self.points["vista_" + br["name"].lower().replace(" ", "_")] = {"pos": [r2(cx), deck, r2(cz)], "yaw": round(yaw, 1)}

    def park(self):
        p = self.cfg["park"]
        cx, cz = self.at(p["bearing"], p["distance"])
        sx, sz = p["size"]
        self.park_rect = (cx - sx / 2, cx + sx / 2, cz - sz / 2, cz + sz / 2)
        self.box(cx, 0, cz, sx, 0.12, sz, [0.27, 0.36, 0.2], "grass")
        self.box(cx, 0, cz, sx, 0.13, 4, [0.55, 0.5, 0.42], "gravel")
        self.box(cx, 0, cz, 4, 0.13, sz, [0.55, 0.5, 0.42], "gravel")
        self.box(cx + sx * 0.2, 0, cz - sz * 0.15, 30, 0.14, 18, [0.22, 0.3, 0.33], "water", roughness=0.1)
        for _ in range(p["trees"]):
            tx, tz = cx + self.rng.uniform(-sx / 2 + 4, sx / 2 - 4), cz + self.rng.uniform(-sz / 2 + 4, sz / 2 - 4)
            if abs(tx - cx) < 4 or abs(tz - cz) < 4:
                continue
            h = self.rng.uniform(7, 13)
            g = self.rng.uniform(0.75, 1.1)
            self.box(tx, 0, tz, 0.5, h * 0.5, 0.5, [0.3, 0.24, 0.18], "wood")
            for k in range(2):
                s = self.rng.uniform(3.5, 6.0) * (1 - 0.3 * k)
                self.box(tx, h * (0.38 + 0.3 * k), tz, s, h * 0.35, s, [0.2 * g, 0.32 * g, 0.15 * g], "grass", self.rng.uniform(0, 90))
        self.points["vista_birds_park"] = {"pos": [r2(cx), 40.0, r2(cz)], "yaw": 0}

    # ---------------------------------------------------------------- blocks
    def blocked(self, x, z, rad):
        ot = self.cfg["own_tower"]
        if ot["min"][0] - rad < x < ot["max"][0] + rad and ot["min"][1] - rad < z < ot["max"][1] + rad:
            return True
        x0, x1, z0, z1 = self.park_rect
        if x0 - rad < x < x1 + rad and z0 - rad < z < z1 + rad:
            return True
        for ex, ez, er in self.exclude:
            if math.hypot(x - ex, z - ez) < er + rad:
                return True
        rv = self.cfg["river"]
        return self.river_dist(x, z) < rv["width"] / 2 + rv["embankment"] + rad + 4

    def blocks(self):
        g = self.cfg["grid"]
        pitch, street, walk = g["pitch"], g["street"], g["sidewalk"]
        ox, oz = g["origin"]
        near0, near1 = self.cfg["bands"]["near"]
        mid1 = self.cfg["bands"]["mid"][1]
        nk = self.cfg["near_kit"]
        n = int(mid1 / pitch) + 2
        kit_lots = []
        for i in range(-n, n + 1):
            for j in range(-n, n + 1):
                bx, bz = ox + (i + 0.5) * pitch, oz + (j + 0.5) * pitch
                bear, d = self.polar(bx, bz)
                if d > mid1 or d < near0 * 0.5:
                    continue
                inner = pitch - street
                if self.blocked(bx, bz, inner * 0.5):
                    # Partly blocked: lots one by one (river banks, the park edge).
                    pass
                elif d < mid1 * 0.9:
                    self.box(bx, 0, bz, inner, 0.15, inner, [0.36, 0.36, 0.35], "concrete")
                style = self.style_at(bear)
                lots = self.rng.choice([(1, 1), (2, 1), (1, 2), (2, 2), (2, 2)])
                lw, ld = (inner - 2 * walk) / lots[0], (inner - 2 * walk) / lots[1]
                for a in range(lots[0]):
                    for c in range(lots[1]):
                        lx = bx - (inner - 2 * walk) / 2 + (a + 0.5) * lw
                        lz = bz - (inner - 2 * walk) / 2 + (c + 0.5) * ld
                        lb, ldist = self.polar(lx, lz)
                        if self.blocked(lx, lz, max(lw, ld) * 0.5) or self.rng.random() < 0.06:
                            continue
                        near = ldist < near1
                        if (near and nk["bearings"][0] <= lb <= nk["bearings"][1] and near0 < ldist < nk["max_distance"]):
                            kit_lots.append((ldist, lx, lz, lw, ld, style))
                            continue
                        self.box_building(lx, lz, lw, ld, style, near, ldist)
        # The nearest lots in view become kit buildings; the rest of the list falls back to boxes.
        kit_lots.sort(key=lambda t: t[0])
        for k, (dist, lx, lz, lw, ld, style) in enumerate(kit_lots):
            if k < nk["max"]:
                self.kit_building(lx, lz, lw, ld, style)
            else:
                self.box_building(lx, lz, lw, ld, style, True, dist)

    def building_height(self, style, near):
        lo, hi = style["near" if near else "mid"]
        if self.rng.random() < style["tall"] * (0.4 if near else 1.0):
            lo, hi = style["tall_h"]
        return self.rng.uniform(lo, hi)

    def box_building(self, x, z, lw, ld, style, near, dist):
        sb = self.rng.uniform(1.0, 3.0)
        w, d = lw - sb, ld - sb
        h = self.building_height(style, near)
        col = list(self.rng.choice(style["colors"]))
        j = self.rng.uniform(0.9, 1.08)
        col = [min(1.0, c * j) for c in col]
        self.box(x, 0, z, w, h, d, col, "brick" if col[0] > col[2] + 0.1 else "concrete", facade=True)
        if self.rng.random() < 0.35 and h > 20:   # setback / penthouse
            self.box(x + self.rng.uniform(-2, 2), h, z + self.rng.uniform(-2, 2), w * 0.6, self.rng.uniform(3, 9), d * 0.6, col, "concrete", facade=True)
        if near or dist < 700:
            self.box(x, h, z, w + 0.4, 0.8, d + 0.4, [c * 0.8 for c in col], "concrete")   # parapet cap
            if self.rng.random() < 0.5:
                self.box(x + self.rng.uniform(-w / 4, w / 4), h + 0.8, z + self.rng.uniform(-d / 4, d / 4), 3, 2.5, 3, [0.45, 0.45, 0.46], "metal_thin")

    # ---------------------------------------------------------------- kit buildings (batched prefab)
    def kit_face(self, x0, x1, z0, z1, floors, mat, variant, ground, top_y, rngb):
        """Facade bays around the rectangle (sizes multiples of 3 m): front faces outward."""
        bx, bz = int(round((x1 - x0) / 3)), int(round((z1 - z0) / 3))
        faces = [(0, bx, lambda i: (x0 + 3 * i, z0 + 0.3)),          # N: yaw 0, along +X
                 (180, bx, lambda i: (x1 - 3 * i, z1 - 0.3)),        # S: along -X
                 (-90, bz, lambda i: (x1 - 0.3, z0 + 3 * i)),        # E: along +Z
                 (90, bz, lambda i: (x0 + 0.3, z1 - 3 * i))]         # W: along -Z
        slot = "M_" + mat
        for yaw, n, pos in faces:
            for i in range(n):
                px, pz = pos(i)
                y = 0.0
                if ground:
                    gp = "fac_shopfront_" + rngb.choice("abc") if (i % 3 or n < 3) else "fac_entrance_" + rngb.choice("ab")
                    self.kit_inst(gp, px, y, pz, yaw, {})
                    y = 4.0
                for f in range(floors):
                    r = rngb.random()
                    piece = "fac_%s_%s" % (mat, "balcony" if r < 0.12 else ("blank" if r < 0.18 else "win_" + variant))
                    self.kit_inst(piece, px, y, pz, yaw, {slot: rngb.choice(["default", "default", "yellow", "grey"]) if mat == "brick" else "default"})
                    y += 3.0
                self.kit_inst("fac_cornice_top", px, y, pz, yaw, {})

    def kit_inst(self, piece, x, y, z, yaw, mat):
        e = {"piece": piece, "pos": [r2(x), r2(y), r2(z)]}
        if yaw:
            e["yaw"] = yaw
        if mat:
            e["mat"] = mat
        self.kit.append(e)

    def kit_building(self, x, z, lw, ld, style, floors=None, mat=None, variant=None, ground=None):
        rngb = random.Random(self.rng.random())
        bx, bz = max(2, min(7, int((lw - 2) // 3))), max(2, min(7, int((ld - 2) // 3)))   # <= 21 m: a lot's street-side building
        x0, z0 = x - bx * 1.5, z - bz * 1.5
        x1, z1 = x0 + bx * 3, z0 + bz * 3
        if floors is None:
            floors = max(3, int(self.building_height(style, True) / 3.0))
        mat = mat or rngb.choice(style["kit"])
        variant = variant or rngb.choice("abc")
        ground = rngb.random() < 0.6 if ground is None else ground
        top = (4.0 if ground else 0.0) + floors * 3.0
        self.kit_face(x0, x1, z0, z1, floors, mat, variant, ground, top, rngb)
        # Roof and a dark core (the panes are opaque; the core stops gaps reading as sky at grazing angles).
        self.box((x0 + x1) / 2, top - 0.1, (z0 + z1) / 2, x1 - x0 - 0.5, 0.5, z1 - z0 - 0.5, [0.42, 0.41, 0.4], "concrete")
        self.box((x0 + x1) / 2, 0, (z0 + z1) / 2, x1 - x0 - 0.7, top - 0.1, z1 - z0 - 0.7, [0.08, 0.08, 0.09], "concrete")
        if rngb.random() < 0.6:
            self.box((x0 + x1) / 2 + rngb.uniform(-2, 2), top + 0.4, (z0 + z1) / 2 + rngb.uniform(-2, 2), 3.0, 2.6, 3.0, [0.5, 0.5, 0.5], "concrete")
        self.stats["kit_buildings"] += 1
        return top

    # ---------------------------------------------------------------- landmarks
    def compress(self, d):
        c = self.cfg["compress"]
        if d <= c["from"]:
            return d
        return c["from"] + c["span"] * (1.0 - math.exp(-(d - c["from"]) / c["scale"]))

    def landmark(self, lm):
        b, d = lm["bearing"], lm["distance"]
        tx, tz = self.at(b, d)
        d2 = self.compress(d)
        k = d2 / d
        start = len(self.brushes)
        getattr(self, "lm_" + lm["kind"])(lm, tx, tz)
        c = self.cfg["compress"]
        fog = 1.0 - math.exp(-(d - d2) * c["fog_density"])
        for br in self.brushes[start:]:
            if k < 0.999:
                (x, y0, z), (sx, sy, sz) = br["pos"], br["size"]
                nx, nz = self.vx + (x - self.vx) * k, self.vz + (z - self.vz) * k
                ny0, ny1 = self.vy + (y0 - self.vy) * k, self.vy + (y0 + sy - self.vy) * k
                if y0 <= 0.05:
                    ny0 = 0.0   # bases reach the ground
                br["pos"] = [r2(nx), r2(ny0), r2(nz)]
                br["size"] = [r2(sx * k), r2(ny1 - ny0), r2(sz * k)]
            if fog > 0.01:
                br["color"] = [round(cc * (1 - fog) + f * fog, 3) for cc, f in zip(br["color"], c["fog"])]
        self.stats["landmarks"] += 1
        ext = [abs(v) for br in self.brushes[start:] for v in (br["pos"][0] - self.vx - (tx - self.vx) * k, br["pos"][2] - self.vz - (tz - self.vz) * k)]
        self.exclude.append((self.vx + (tx - self.vx) * k, self.vz + (tz - self.vz) * k, max(ext + [10.0]) + 15.0))
        return k, (self.vx + (tx - self.vx) * k, self.vz + (tz - self.vz) * k)

    def tf_point(self, k, x, y, z):
        return [r2(self.vx + (x - self.vx) * k), r2(max(0.0, self.vy + (y - self.vy) * k)), r2(self.vz + (z - self.vz) * k)]

    def lm_kit_tower(self, lm, x, z):
        bx, bz = lm["bays"]
        floors = int(lm["height"] / 3.0) - 1
        top = self.kit_building(x, z, bx * 3 + 2.5, bz * 3 + 2.5, None, floors=floors, mat=lm["material"], variant=lm["window"], ground=False)
        self.box(x, top + 0.4, z, 8, 4, 6, [0.45, 0.45, 0.47], "concrete")
        self.box(x + 2, top + 4.4, z, 0.3, 12, 0.3, [0.6, 0.15, 0.1], "metal_thin")

    def lm_crane(self, lm, x, z):
        h, jib = lm["height"], lm["jib"]
        yel = [0.85, 0.62, 0.1]
        self.box(x, 0, z, 2.4, h, 2.4, yel, "metal_thin")
        a = math.radians(lm["jib_yaw"])
        dx, dz = math.sin(a), -math.cos(a)
        self.box(x + dx * jib * 0.42, h, z + dz * jib * 0.42, 1.6, 1.8, jib, yel, "metal_thin", -lm["jib_yaw"])
        self.box(x - dx * 9, h, z - dz * 9, 1.6, 1.6, 16, yel, "metal_thin", -lm["jib_yaw"])
        self.box(x - dx * 15, h - 2.5, z - dz * 15, 3.5, 3.0, 4.0, [0.5, 0.5, 0.48], "concrete", -lm["jib_yaw"])
        self.box(x, h + 1.8, z, 1.2, 7, 1.2, yel, "metal_thin")
        self.box(x + dx * 2, h - 3, z + dz * 2, 2.5, 3, 2.5, [0.8, 0.8, 0.78], "metal_thin", -lm["jib_yaw"])
        hx, hz = x + dx * jib * 0.7, z + dz * jib * 0.7
        self.box(hx, h * 0.45, hz, 0.1, h * 0.55, 0.1, [0.2, 0.2, 0.2], "metal_thin")
        if lm.get("site"):
            sx, sz = x + 14, z + 4
            core = h * 0.5
            self.box(sx, 0, sz, 8, core + 4, 8, [0.6, 0.6, 0.58], "concrete")
            for f in range(int(core / 3.5)):
                self.box(sx, f * 3.5 + 3.3, sz, 30, 0.3, 24, [0.62, 0.62, 0.6], "concrete")
            for cx in (-14, 14):
                for cz in (-11, 11):
                    self.box(sx + cx, 0, sz + cz, 0.6, core, 0.6, [0.6, 0.6, 0.58], "concrete")

    def lm_hospital(self, lm, x, z):
        h = lm["height"]
        white = [0.84, 0.84, 0.8]
        self.box(x - 40, 0, z, 18, h, 90, white, "plaster", facade=True)
        self.box(x + 40, 0, z, 18, h, 90, white, "plaster", facade=True)
        self.box(x, 0, z, 62, h * 0.7, 20, [0.78, 0.8, 0.8], "plaster", facade=True)
        self.box(x, h * 0.7, z, 20, 0.6, 20, [0.3, 0.3, 0.3], "concrete")             # helipad
        self.box(x, h * 0.7 + 0.6, z, 12, 0.05, 2, [0.9, 0.9, 0.9], "concrete")
        for s in (-1, 1):
            self.box(x - 40, h, z + s * 30, 10, 3, 8, [0.6, 0.6, 0.6], "metal_thin")
            self.box(x + 40, h - 6, z + s * 46, 1.0, 5, 6, [0.8, 0.1, 0.1], "plaster", emissive=1.5)   # red cross signs
        self.hosp_roof = (x + 40, h, z - 20)

    def lm_spire(self, lm, x, z):
        glass = [0.42, 0.52, 0.62]
        self.box(x, 0, z, 80, 22, 80, [0.55, 0.57, 0.6], "concrete", facade=True)
        tiers = [(46, 70), (42, 60), (37, 50), (31, 40), (24, 30), (16, 20)]
        y = 22.0
        for i, (s, th) in enumerate(tiers):
            self.box(x, y, z, s, th, s, glass, "glass", i * 9.0, roughness=0.18, metallic=0.55, facade=True)
            y += th
        self.box(x, y, z, 2.5, lm["height"] - y, 2.5, [0.7, 0.72, 0.75], "metal_thin", metallic=0.8, roughness=0.3)
        self.box(x, lm["height"] - 2, z, 0.8, 2, 0.8, [1.0, 0.2, 0.15], "metal_thin", emissive=4.0)   # aircraft light

    def lm_cluster(self, lm, x, z):
        rng = random.Random(lm["bearing"] * 7 + lm["distance"])
        for i in range(lm["count"]):
            a = rng.uniform(0, math.tau)
            r = rng.uniform(0, lm["spread"] * 0.5) if i else 0
            tx, tz = x + math.cos(a) * r, z + math.sin(a) * r
            h = lm["height"] * (1.0 if i == 0 else rng.uniform(0.4, 0.85))
            s = rng.uniform(30, 48)
            col = rng.choice([[0.4, 0.48, 0.56], [0.55, 0.58, 0.6], [0.32, 0.38, 0.44], [0.62, 0.6, 0.55]])
            yaw = rng.choice([0, 0, 15, 30, 45])
            self.box(tx, 0, tz, s, h, s * rng.uniform(0.7, 1.0), col, "glass", yaw, roughness=0.2, metallic=0.5, facade=True)
            if rng.random() < 0.6:
                self.box(tx, h, tz, s * 0.55, h * 0.12, s * 0.55, col, "glass", yaw, roughness=0.2, metallic=0.5, facade=True)

    def lm_stadium(self, lm, x, z):
        r, h = lm["radius"], lm["height"]
        n = 20
        for i in range(n):
            a = 360.0 * i / n
            px, pz = x + math.sin(math.radians(a)) * r, z - math.cos(math.radians(a)) * r
            L = 2 * math.pi * r / n + 3
            self.box(px, 0, pz, L, h * 0.75, 26, [0.6, 0.6, 0.62], "concrete", -a)
            px2, pz2 = x + math.sin(math.radians(a)) * (r - 18), z - math.cos(math.radians(a)) * (r - 18)
            self.box(px2, h * 0.9, pz2, L * 0.9, 1.5, 34, [0.85, 0.86, 0.88], "metal_thin", -a)
        for a in (45, 135, 225, 315):
            px, pz = x + math.sin(math.radians(a)) * (r + 6), z - math.cos(math.radians(a)) * (r + 6)
            self.box(px, 0, pz, 2, h * 1.6, 2, [0.5, 0.5, 0.5], "metal_thin")
            self.box(px, h * 1.6, pz, 8, 3, 1.5, [0.9, 0.9, 0.85], "metal_thin", -a, emissive=0.5)

    # ---------------------------------------------------------------- skyline ring, roads, anchors
    def skyline(self):
        s0, s1 = self.cfg["bands"]["skyline"]
        c = self.cfg["compress"]
        b = -180.0
        while b < 180:
            d = self.rng.uniform(s0, s1)
            x, z = self.at(b, d)
            if self.river_dist(x, z) > self.cfg["river"]["width"] * 0.6 + 20:
                style = self.style_at(b)
                h = self.rng.uniform(10, 32) * (1.4 if self.rng.random() < 0.15 else 1.0)
                col = list(self.rng.choice(style["colors"]))
                fog = 0.35
                col = [cc * (1 - fog) + f * fog for cc, f in zip(col, c["fog"])]
                w = self.rng.uniform(25, 60)
                self.box(x, 0, z, w, h, self.rng.uniform(25, 50), col, "concrete", -b, facade=True)
            b += self.rng.uniform(1.2, 2.6)

    def arterials(self):
        """Streets for traffic (vista sequences): the grid lines in the window's view, split at the river."""
        g = self.cfg["grid"]
        pitch = g["pitch"]
        ox, oz = g["origin"]
        lines = []
        for i in range(-3, 9):          # north-south streets east of the tower
            x = ox + i * pitch
            lines.append([(x, oz - 50 - k * 30) for k in range(36)])
        for j in range(-8, 2):          # east-west streets to the north
            z = oz + j * pitch
            lines.append([(ox - 200 + k * 30, z) for k in range(40)])
        for ln in lines:
            run = []
            for (x, z) in ln + [(None, None)]:
                ok = x is not None and self.polar(x, z)[1] < 1000 and not self.blocked(x, z, 0) and self.polar(x, z)[1] > 60
                if ok:
                    run.append([r2(x), 0.0, r2(z)])
                elif len(run) >= 6:
                    self.roads.append({"name": "street_%d" % len(self.roads), "points": run})
                    run = []
                else:
                    run = []

    def anchors(self, hosp_k, hosp_c):
        hx, hy, hz = self.hosp_roof
        P = self.points
        P["vista_hospital"] = {"pos": self.tf_point(hosp_k, hx, hy, hz), "yaw": 0}
        P["heli_crash_anchor"] = P["vista_hospital"]
        P["smoke_hospital"] = {"pos": self.tf_point(hosp_k, hx, 0, hz), "yaw": 0}
        for name, b, d in (("smoke_2", -32, 900), ("smoke_3", 104, 700)):
            x, z = self.at(b, d)
            P[name] = {"pos": [r2(x), 0.0, r2(z)], "yaw": 0}
        # Helicopter paths (beat 2 far north, west to east; beat 5 in from the north-east to the hospital).
        for name, b, d, y in (("heli1_a", -70, 1300, 140), ("heli1_b", 60, 1300, 140), ("heli2_a", 5, 1250, 160),
                              ("heli3_a", 120, 1200, 120), ("heli3_b", -20, 900, 110)):
            x, z = self.at(b, d)
            P[name] = {"pos": [r2(x), y, r2(z)], "yaw": 0}
        for name, b, d in (("siren_1", -15, 600), ("siren_2", 40, 750), ("siren_3", 95, 500)):
            x, z = self.at(b, d)
            P[name] = {"pos": [r2(x), 2.0, r2(z)], "yaw": 0}
        x, z = self.at(-10, self.river_r(-10))
        P["vista_birds_river"] = {"pos": [r2(x), 25.0, r2(z)], "yaw": 0}

    def generate(self):
        self.water()
        self.bridges()
        self.park()
        lm_k = {}
        for lm in self.cfg["landmarks"]:
            k, c = self.landmark(lm)
            lm_k[lm["kind"] + ":" + lm["name"]] = (k, c)
            self.points["vista_" + lm["name"].lower().replace(" ", "_").replace(".", "")] = {"pos": [r2(c[0]), 0.0, r2(c[1])], "yaw": 0}
        self.blocks()
        self.skyline()
        self.arterials()
        hosp = [v for kk, v in lm_k.items() if kk.startswith("hospital:")][0]
        self.anchors(*hosp)
        return self


def generate(path=SRC):
    with open(path) as f:
        cfg = json.load(f)
    c = City(cfg).generate()
    city_block = {"_doc": "City window lights (src/world/level.c, render_set_city): base lit fraction at night, intensity, "
                          "power sectors are 45 deg of bearing from 'center' (0 = N..45, 1 = 45..90 (Halden), ...). "
                          "Roads: polylines for vista traffic. GENERATED by tools/story/vista_gen.py.",
                  "base": 0.32, "intensity": 1.6, "center": [cfg["view"][0], cfg["view"][2]], "roads": c.roads}
    prefab = {"prefab": NEAR_PREFAB, "_doc": "The near city's kit facade buildings (Vellmouth, assets/story/vista/vellmouth.json). "
                                             "GENERATED by tools/story/vista_gen.py; placed with \"batch\": true.", "pieces": c.kit}
    return {"brushes": c.brushes, "prefab": prefab, "city": city_block, "points": c.points, "stats": c.stats}


if __name__ == "__main__":
    out = generate()
    s = out["stats"]
    print("vellmouth: %d boxes, %d kit buildings (%d pieces), %d landmarks, %d roads, %d points"
          % (s["boxes"], s["kit_buildings"], len(out["prefab"]["pieces"]), s["landmarks"], len(out["city"]["roads"]), len(out["points"])))
