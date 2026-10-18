"""Building kit generator (docs/BUILDING_KIT.md): writes the first piece set as .glb files that follow the
kit conventions, the material sets and the manifest.

    python tools/kit_gen/kit_gen.py            # assets/kits/building/{*.glb, kit.json, materials/*.json}

Every piece is boxes (walls, slabs, frames, furniture blocks) so the set is complete and consistent
before artists replace pieces in Blender (import the .glb, edit, export with tools/blender/kit_export.py).
Per piece: LOD0 named like the file, LOD1 when small trims can be dropped, UBX_<id>_NN box collision,
S_<type> socket empties, material slots M_<set>, UV0 at world scale (1 UV unit = 1 m = 1024 px:
10.24 px/cm), UV1 packed without overlaps (lightmaps later). Engine space = glTF space: +Y up, the
front faces -Z, pivot at the bottom-left-back corner (x 0..w, y 0..h, z -d..0).
"""
import json, math, os, struct, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OUT = os.path.join(ROOT, "assets", "kits", "building")

# ------------------------------------------------------------------ materials
MATERIALS = {   # set -> surface (impacts / footsteps) + variants (colour, roughness, metallic, emissive)
    "paint":    ("plaster", {"default": [0.86, 0.84, 0.80, 0.85, 0.0], "warm_white": [0.90, 0.87, 0.80, 0.85, 0.0], "sage": [0.62, 0.68, 0.58, 0.85, 0.0],
                             "blush": [0.86, 0.72, 0.72, 0.85, 0.0], "slate": [0.42, 0.46, 0.52, 0.85, 0.0], "corridor": [0.78, 0.76, 0.70, 0.8, 0.0]}),
    "wallpaper": ("plaster", {"default": [0.80, 0.74, 0.64, 0.9, 0.0], "stars": [0.30, 0.36, 0.55, 0.9, 0.0]}),
    "wood":     ("wood", {"default": [0.55, 0.42, 0.30, 0.6, 0.0], "oak_light": [0.70, 0.56, 0.40, 0.55, 0.0], "walnut": [0.36, 0.25, 0.18, 0.5, 0.0],
                          "white": [0.92, 0.91, 0.88, 0.5, 0.0], "floor": [0.58, 0.44, 0.31, 0.45, 0.0]}),
    "tile":     ("tile", {"default": [0.86, 0.87, 0.88, 0.25, 0.0], "dark": [0.30, 0.31, 0.33, 0.3, 0.0], "green": [0.45, 0.58, 0.52, 0.25, 0.0]}),
    "concrete": ("concrete", {"default": [0.55, 0.55, 0.53, 0.9, 0.0], "light": [0.70, 0.70, 0.68, 0.9, 0.0]}),
    "brick":    ("brick", {"default": [0.55, 0.33, 0.25, 0.9, 0.0], "yellow": [0.72, 0.62, 0.42, 0.9, 0.0], "grey": [0.48, 0.46, 0.44, 0.9, 0.0]}),
    "stone":    ("concrete", {"default": [0.74, 0.70, 0.62, 0.85, 0.0]}),
    "render":   ("plaster", {"default": [0.80, 0.78, 0.72, 0.9, 0.0], "cream": [0.88, 0.84, 0.72, 0.9, 0.0]}),
    "glass":    ("glass", {"default": [0.70, 0.85, 0.92, 0.05, 0.0], "frosted": [0.85, 0.88, 0.90, 0.4, 0.0]}),
    "metal":    ("metal_thin", {"default": [0.20, 0.20, 0.22, 0.4, 0.8], "steel": [0.60, 0.62, 0.64, 0.35, 1.0], "white": [0.90, 0.90, 0.90, 0.4, 0.3]}),
    "trim":     ("wood", {"default": [0.94, 0.93, 0.90, 0.5, 0.0]}),
    "fabric":   ("cloth", {"default": [0.40, 0.42, 0.48, 0.95, 0.0], "cream": [0.82, 0.78, 0.70, 0.95, 0.0], "red": [0.60, 0.20, 0.18, 0.95, 0.0],
                           "teal": [0.20, 0.45, 0.48, 0.95, 0.0]}),
    "plastic":  ("default", {"default": [0.12, 0.12, 0.13, 0.4, 0.0], "white": [0.92, 0.92, 0.92, 0.4, 0.0], "toy": [0.85, 0.30, 0.25, 0.4, 0.0]}),
    "carpet":   ("cloth", {"default": [0.50, 0.46, 0.42, 1.0, 0.0]}),
    "emissive": ("glass", {"default": [1.00, 0.96, 0.88, 0.5, 0.0, 2.5], "screen": [0.30, 0.45, 0.70, 0.3, 0.0, 1.2], "exit": [0.10, 0.85, 0.30, 0.5, 0.0, 3.0]}),
    "window":   ("glass", {"default": [0.10, 0.12, 0.14, 0.08, 0.6], "warm": [0.14, 0.12, 0.10, 0.1, 0.5]}),   # opaque façade panes, lit at night
}

# ------------------------------------------------------------------ geometry
class Piece:
    def __init__(self, pid, cat, **meta):
        self.id, self.cat, self.meta = pid, cat, meta
        self.boxes = []      # (lo, hi, slot, collide, lod1)
        self.sockets = []    # (name, pos, yaw_deg)

    def box(self, x0, y0, z0, x1, y1, z1, slot, collide=True, lod1=None):
        lo, hi = (min(x0, x1), min(y0, y1), min(z0, z1)), (max(x0, x1), max(y0, y1), max(z0, z1))
        if lod1 is None:   # small trims vanish at LOD1
            lod1 = min(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) >= 0.05 or max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) >= 0.6
        self.boxes.append((lo, hi, slot, collide, lod1))
        return self

    def socket(self, stype, x, y, z, yaw=0.0):
        name = "S_" + stype
        k = 2
        while any(s[0] == name for s in self.sockets):
            name = "S_%s_%d" % (stype, k)
            k += 1
        self.sockets.append((name, (x, y, z), yaw))
        return self


FACES = [  # axis, sign, (u axis, v axis)
    (0, 1, (2, 1)), (0, -1, (2, 1)), (1, 1, (0, 2)), (1, -1, (0, 2)), (2, 1, (0, 1)), (2, -1, (0, 1)),
]


def box_faces(lo, hi):
    """Six quads with outward normals; CCW seen from outside. Yields (normal, [4 corners], (w, h))."""
    for axis, sign, (ua, va) in FACES:
        c = [0, 0, 0]
        c[axis] = hi[axis] if sign > 0 else lo[axis]
        n = [0, 0, 0]
        n[axis] = float(sign)
        quad = []
        for (uu, vv) in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = list(c)
            p[ua] = hi[ua] if uu else lo[ua]
            p[va] = hi[va] if vv else lo[va]
            quad.append(p)
        # Orient CCW around the outward normal.
        e1 = [quad[1][i] - quad[0][i] for i in range(3)]
        e2 = [quad[2][i] - quad[0][i] for i in range(3)]
        cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        if sum(cr[i] * n[i] for i in range(3)) < 0:
            quad = [quad[0], quad[3], quad[2], quad[1]]
        yield n, quad, (hi[ua] - lo[ua], hi[va] - lo[va]), (ua, va)


def pack_uv1(rects):
    """Shelf-pack (w, h) rects into 0..1 with a 2 px margin at 512; returns [(u0, v0, scale)]."""
    margin = 2.0 / 512.0
    total = sum(w * h for w, h in rects) or 1.0
    scale = 0.85 / math.sqrt(total)
    for _ in range(40):
        order = sorted(range(len(rects)), key=lambda i: -rects[i][1])
        x = y = shelf = 0.0
        place = [None] * len(rects)
        ok = True
        for i in order:
            w, h = rects[i][0] * scale + margin, rects[i][1] * scale + margin
            if x + w > 1.0:
                x, y, shelf = 0.0, y + shelf, 0.0
            if y + h > 1.0 or w > 1.0:
                ok = False
                break
            place[i] = (x + margin * 0.5, y + margin * 0.5)
            x += w
            shelf = max(shelf, h)
        if ok:
            return [(p[0], p[1], scale) for p in place]
        scale *= 0.92
    raise RuntimeError("uv1 packing failed")


def mesh_from_boxes(boxes, uv_scale=1.0):
    """slot -> (positions, normals, uv0, uv1, indices) for a list of boxes; UV1 packed over all faces."""
    faces = []
    for lo, hi, slot, _, _ in boxes:
        for n, quad, wh, axes in box_faces(lo, hi):
            if wh[0] < 1e-6 or wh[1] < 1e-6:
                continue
            faces.append((slot, n, quad, wh, axes))
    packing = pack_uv1([(f[3][0], f[3][1]) for f in faces])
    out = {}
    for (slot, n, quad, wh, (ua, va)), (u0, v0, sc) in zip(faces, packing):
        P, N, T0, T1, I = out.setdefault(slot, ([], [], [], [], []))
        base = len(P) // 3
        lo_u = min(q[ua] for q in quad)
        lo_v = min(q[va] for q in quad)
        for q in quad:
            P.extend(q)
            N.extend(n)
            T0.extend([q[ua] * uv_scale, -q[va] * uv_scale])   # world metres (V down, glTF convention)
            T1.extend([u0 + (q[ua] - lo_u) * sc, v0 + (q[va] - lo_v) * sc])
        I.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return out


# ------------------------------------------------------------------ glb writer
def front_container_sockets(piece):
    """Containers are opened from the front: their socket sits just in front of the face, mid height
    (the interaction ray must reach it without hitting the piece's own collision)."""
    if not piece.boxes:
        return
    mx = [max(b[1][i] for b in piece.boxes) for i in range(3)]
    mn = [min(b[0][i] for b in piece.boxes) for i in range(3)]
    for i, (name, pos, yaw) in enumerate(piece.sockets):
        if name.startswith("S_container"):
            piece.sockets[i] = (name, (pos[0], round(min((mx[1] - mn[1]) * 0.5, 0.9), 3), round(mn[2] - 0.03, 3)), yaw)


def write_glb(path, piece, slot_colors, uv_scale=1.0, extra_hulls=(), post=None):
    """extra_hulls: (name, positions, indices) meshes; post(gltf dict): edits before writing (test fixtures)."""
    nodes, meshes, accessors, views, materials, mat_index = [], [], [], [], [], {}
    blob = bytearray()

    def add_view(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        views.append(v)
        return len(views) - 1

    def add_acc(values, comps, is_index=False, minmax=False):
        if is_index:
            data = struct.pack("<%dI" % len(values), *values)
            a = {"bufferView": add_view(data, 34963), "componentType": 5125, "count": len(values), "type": "SCALAR"}
        else:
            data = struct.pack("<%df" % len(values), *values)
            a = {"bufferView": add_view(data, 34962), "componentType": 5126, "count": len(values) // comps, "type": {2: "VEC2", 3: "VEC3"}[comps]}
            if minmax:
                a["min"] = [min(values[i::comps]) for i in range(comps)]
                a["max"] = [max(values[i::comps]) for i in range(comps)]
        accessors.append(a)
        return len(accessors) - 1

    def material(slot):
        if slot not in mat_index:
            c = slot_colors.get(slot, [0.7, 0.7, 0.7, 0.8, 0.0])
            m = {"name": slot, "pbrMetallicRoughness": {"baseColorFactor": [c[0], c[1], c[2], 0.35 if slot == "M_glass" else 1.0],
                                                        "roughnessFactor": c[3], "metallicFactor": c[4]}}
            if slot == "M_glass":
                m["alphaMode"] = "BLEND"
            materials.append(m)
            mat_index[slot] = len(materials) - 1
        return mat_index[slot]

    def add_mesh_node(name, slot_meshes):
        prims = []
        for slot, (P, N, T0, T1, I) in sorted(slot_meshes.items()):
            attrs = {"POSITION": add_acc(P, 3, minmax=True), "NORMAL": add_acc(N, 3), "TEXCOORD_0": add_acc(T0, 2), "TEXCOORD_1": add_acc(T1, 2)}
            prims.append({"attributes": attrs, "indices": add_acc(I, 1, is_index=True), "material": material(slot)})
        meshes.append({"name": name, "primitives": prims})
        nodes.append({"name": name, "mesh": len(meshes) - 1})

    add_mesh_node(piece.id, mesh_from_boxes(piece.boxes, uv_scale))
    lod1 = [b for b in piece.boxes if b[4]]
    if 0 < len(lod1) < len(piece.boxes):
        add_mesh_node(piece.id + "_LOD1", mesh_from_boxes(lod1))
    k = 1
    for lo, hi, slot, collide, _ in piece.boxes:
        if not collide:
            continue
        P, I = [], []
        for n, quad, wh, axes in box_faces(lo, hi):
            base = len(P) // 3
            for q in quad:
                P.extend(q)
            I.extend([base, base + 1, base + 2, base, base + 2, base + 3])
        meshes.append({"name": "UBX_%s_%02d" % (piece.id, k), "primitives": [{"attributes": {"POSITION": add_acc(P, 3, minmax=True)}, "indices": add_acc(I, 1, True)}]})
        nodes.append({"name": "UBX_%s_%02d" % (piece.id, k), "mesh": len(meshes) - 1})
        k += 1
    for name, P_, I_ in extra_hulls:
        meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": add_acc(P_, 3, minmax=True)}, "indices": add_acc(I_, 1, True)}]})
        nodes.append({"name": name, "mesh": len(meshes) - 1})
    for name, pos, yaw in piece.sockets:
        h = math.radians(yaw) * 0.5
        nodes.append({"name": name, "translation": [round(v, 4) for v in pos], "rotation": [0.0, math.sin(h), 0.0, math.cos(h)]})
    gltf = {"asset": {"version": "2.0", "generator": "deadground kit_gen"}, "scene": 0, "scenes": [{"name": piece.id, "nodes": list(range(len(nodes)))}],
            "nodes": nodes, "meshes": meshes, "materials": materials, "accessors": accessors, "bufferViews": views,
            "buffers": [{"byteLength": len(blob)}], "extras": {"category": piece.cat}}
    if post:
        post(gltf)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    while len(blob) % 4:
        blob.append(0)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


# ------------------------------------------------------------------ the piece set
H_INT, H_EXT, T_INT, T_EXT = 2.7, 3.0, 0.2, 0.3
pieces = []


def P(pid, cat, **meta):
    p = Piece(pid, cat, **meta)
    pieces.append(p)
    return p


def wall_run(p, w, h, t, slot_front, slot_back, x0=0.0, y0=0.0):
    p.box(x0, y0, -t, x0 + w, y0 + h, 0, slot_front)
    return p


def opening_wall(pid, cat, w, h, t, ow, oh, sill, slot, ext=False, **meta):
    """A wall module with an opening (door: sill 0) centred; sockets S_open / S_frame at the opening's bottom centre."""
    p = P(pid, cat, **meta)
    j = (w - ow) / 2.0
    p.box(0, 0, -t, j, h, 0, slot)
    p.box(w - j, 0, -t, w, h, 0, slot)
    p.box(j, oh, -t, w - j, h, 0, slot)
    if sill > 0:
        p.box(j, 0, -t, w - j, sill, 0, slot)
    p.socket("open", w / 2.0, sill, -t / 2.0)
    p.socket("frame", w / 2.0, sill, -t / 2.0)
    return p


# Walls
for t, h, kind, slot in ((T_INT, H_INT, "int", "M_paint"), (T_EXT, H_EXT, "ext", "M_render")):
    for w in (0.05, 0.1, 0.5, 1.0, 2.0, 3.0, 4.0):
        P("wall_%s_%03d" % (kind, int(round(w * 100))), "wall", snap="wall").box(0, 0, -t, w, h, 0, slot)
    P("wall_%s_corner_in" % kind, "wall", snap="wall").box(0, 0, -t, t, h, 0, slot)
    P("wall_%s_corner_out" % kind, "wall", snap="wall").box(0, 0, -t, t, h, 0, slot).box(0, 0, -2 * t, t, h, -t, slot)
    P("wall_%s_t" % kind, "wall", snap="wall").box(0, 0, -t, 3 * t, h, 0, slot).box(t, 0, -2 * t, 2 * t, h, -t, slot)
    P("wall_%s_end" % kind, "wall", snap="wall").box(0, 0, -t, t, h, 0, slot).box(0, h - 0.05, -t, t, h, 0, "M_trim", collide=False)
    for ow in (0.9, 1.2, 1.6):
        opening_wall("wall_%s_door_%03d" % (kind, int(ow * 100)), "wall_opening", ow + 0.3, h, t, ow, 2.1, 0, slot, snap="wall", opening="door")
P("wall_int_half_100", "wall", snap="wall").box(0, 0, -T_INT, 1.0, 1.1, 0, "M_paint").box(0, 1.1, -T_INT, 1.0, 1.15, 0.0, "M_wood")
opening_wall("wall_int_pass_150", "wall_opening", 1.5, H_INT, T_INT, 1.2, 2.1, 0.9, "M_paint", snap="wall", opening="pass")
opening_wall("wall_ext_win_150", "wall_opening", 1.5, H_EXT, T_EXT, 1.2, 2.1, 0.9, "M_render", snap="wall", opening="window")
opening_wall("wall_ext_win_tall_150", "wall_opening", 1.5, H_EXT, T_EXT, 1.2, 2.5, 0.3, "M_render", snap="wall", opening="window")
P("wall_party_300", "wall", snap="wall").box(0, 0, -T_EXT, 3.0, H_EXT, 0, "M_paint")
p = opening_wall("wall_corridor_recess_150", "wall_opening", 1.5, H_INT, T_EXT, 1.0, 2.15, 0, "M_paint", snap="wall", opening="door")
p.socket("number_plate", 1.32, 1.6, -T_EXT)

# Full-height glass façade
p = P("glass_panel_150", "glass", snap="mullion")
p.box(0, 0, -0.03, 0.03, H_INT, 0, "M_metal").box(1.47, 0, -0.03, 1.5, H_INT, 0, "M_metal")   # glazing beads (the pane spans mullion to mullion)
p.box(0.03, 0, -0.02, 1.47, H_INT, 0, "M_glass").socket("blinds_rail", 0.75, H_INT - 0.05, -0.12).socket("curtain_rail", 0.75, H_INT - 0.02, -0.2)
P("glass_mullion", "glass", snap="mullion").box(0, 0, -0.15, 0.06, 3.0, 0, "M_metal")
P("glass_mullion_corner", "glass", snap="mullion").box(0, 0, -0.15, 0.15, 3.0, 0, "M_metal")
P("glass_head_150", "glass_trim", snap="mullion").box(0, 0, -0.15, 1.5, 0.3, 0, "M_metal", collide=False)
P("glass_sill_150", "glass_trim", snap="mullion").box(0, 0, -0.15, 1.5, 0.05, 0, "M_metal")
p = P("glass_slider_frame_150", "glass", snap="mullion")
p.box(0, 0, -0.15, 1.5, 0.04, 0, "M_metal").box(0, H_INT - 0.06, -0.15, 1.5, H_INT, 0, "M_metal", collide=False).socket("door", 0.75, 0, -0.075)
p.box(0, 0.04, -0.15, 0.04, H_INT - 0.06, 0, "M_metal").box(1.46, 0.04, -0.15, 1.5, H_INT - 0.06, 0, "M_metal")
P("glass_slider_leaf_075", "door_leaf", snap="socket").box(0, 0, -0.05, 0.75, 2.6, 0, "M_glass").box(0, 0, -0.06, 0.05, 2.6, 0, "M_metal").socket("handle", 0.68, 1.0, -0.06).socket("hinge", 0, 0, -0.025)
P("glass_slider_track_150", "glass_trim", snap="mullion").box(0, 0, -0.15, 1.5, 0.02, 0, "M_metal", collide=False)
P("glass_spandrel_150", "glass", snap="mullion").box(0, 0, -0.02, 1.5, 0.3, 0, "M_glass").box(0, 0, -0.04, 1.5, 0.3, -0.02, "M_metal")
P("balustrade_glass_150", "balcony", snap="grid").box(0, 0, -0.03, 0.02, 1.1, 0, "M_metal").box(1.48, 0, -0.03, 1.5, 1.1, 0, "M_metal").box(0.02, 0, -0.02, 1.48, 1.1, 0, "M_glass")
P("balustrade_glass_120", "balcony", snap="grid").box(0, 0, -0.03, 0.02, 1.1, 0, "M_metal").box(1.18, 0, -0.03, 1.2, 1.1, 0, "M_metal").box(0.02, 0, -0.02, 1.18, 1.1, 0, "M_glass")
P("balustrade_post", "balcony", snap="grid").box(0, 0, -0.06, 0.06, 1.12, 0, "M_metal")
P("balustrade_corner_post", "balcony", snap="grid").box(0, 0, -0.08, 0.08, 1.12, 0, "M_metal")
P("handrail_150", "balcony", snap="grid").box(0, 0, -0.06, 1.5, 0.04, 0, "M_metal")
P("balcony_slab_150x120", "floor", snap="grid").box(0, 0, -1.2, 1.5, 0.3, 0, "M_concrete")
P("balcony_slab_corner_120", "floor", snap="grid").box(0, 0, -1.2, 1.2, 0.3, 0, "M_concrete")
P("balcony_soffit_150x120", "floor_trim", snap="grid").box(0, 0, -1.2, 1.5, 0.02, 0, "M_render", collide=False)
P("balcony_drip_150", "floor_trim", snap="grid").box(0, 0, -0.05, 1.5, 0.05, 0, "M_metal", collide=False)

# Floors / ceilings
for w, d in ((1.0, 1.0), (2.0, 2.0), (4.0, 4.0), (1.0, 0.5)):
    P("floor_slab_%03dx%03d" % (int(w * 100), int(d * 100)), "floor", snap="grid").box(0, 0, -d, w, 0.3, 0, "M_concrete")
FINISH_SIZES = (2.0, 1.0, 0.5, 0.2, 0.1)   # any room on the 0.1 m grid tiles with these (tools/story: greedy per axis)
for fin, slot in (("wood", "M_wood"), ("tile", "M_tile"), ("carpet", "M_carpet")):
    for w in FINISH_SIZES:
        for d in FINISH_SIZES:
            if fin == "carpet" and (w < 1.0 or d < 1.0):
                continue
            P("floor_finish_%s_%03dx%03d" % (fin, int(round(w * 100)), int(round(d * 100))), "floor_finish", snap="grid").box(0, 0, -d, w, 0.02, 0, slot, collide=False)
for w in (1.0, 2.0, 4.0):
    p = P("ceiling_%03d" % int(w * 100), "ceiling", snap="grid").box(0, 0, -w, w, 0.02, 0, "M_paint")
    p.socket("light", w / 2.0, 0, -w / 2.0, 0)
P("ceiling_bulkhead_100", "ceiling", snap="grid").box(0, 0, -1.0, 1.0, 0.3, 0, "M_paint")
P("ceiling_tile_corridor_060", "ceiling", snap="grid").box(0, 0, -0.6, 0.6, 0.02, 0, "M_paint", collide=False).box(0, 0, -0.6, 0.6, 0.025, -0.59, "M_metal", collide=False)
P("landing_slab_stair", "floor", snap="grid").box(0, 0, -1.5, 3.5, 0.3, 0, "M_concrete")

# Stairs (9 risers per half storey, going up towards -Z)
p = P("stair_flight_150x150", "stair", snap="grid")
for k in range(9):
    p.box(0, 0, -(k + 1) * 0.28, 1.5, (k + 1) * (1.5 / 9), -k * 0.28, "M_concrete")
P("stair_half_landing_300x150", "stair", snap="grid").box(0, 0, -1.5, 3.0, 0.3, 0, "M_concrete")
p = P("stair_rail_flight", "stair_rail", snap="socket")
p.box(0, 0.9, -0.05, 0.05, 2.4, 0, "M_metal", collide=False, lod1=True)
for k in range(5):
    p.box(0, 0, -0.03 - k * 0.6, 0.03, 0.9 + k * 0.3, -k * 0.6, "M_metal", collide=False)
P("stair_rail_landing", "stair_rail", snap="socket").box(0, 0.9, -0.05, 1.5, 0.95, 0, "M_metal").box(0, 0, -0.04, 0.04, 0.9, 0, "M_metal", collide=False)
P("stair_rail_end", "stair_rail", snap="socket").box(0, 0, -0.08, 0.08, 1.0, 0, "M_metal")
P("stair_newel", "stair_rail", snap="socket").box(0, 0, -0.1, 0.1, 1.05, 0, "M_metal")
P("stair_stringer_flight", "stair", snap="grid").box(0, 0, -2.52, 0.05, 1.7, 0, "M_concrete", collide=False)
P("stair_eye_impostor", "stair", snap="grid").box(0, 0, -0.01, 1.0, 6.0, 0, "M_concrete", collide=False)

# Doors: frames (with the door entity's socket) and leaves
for kind, ow, oh, slot in (("int", 0.9, 2.1, "M_trim"), ("entr", 0.9, 2.1, "M_metal"), ("fire", 0.9, 2.1, "M_metal")):
    p = P("door_frame_%s_%03d" % (kind, int(ow * 100)), "door_frame", snap="opening", door_kind=kind)
    p.box(0, 0, -0.22, 0.05, oh + 0.05, 0, slot).box(ow + 0.05, 0, -0.22, ow + 0.1, oh + 0.05, 0, slot).box(0.05, oh, -0.22, ow + 0.05, oh + 0.05, 0, slot)
    p.socket("door", (ow + 0.1) / 2.0, 0, -0.11).socket("hinge", 0.05, 0, -0.11)
p = P("door_frame_slide_090", "door_frame", snap="opening", door_kind="slide")
p.box(0, 2.1, -0.22, 1.9, 2.18, 0, "M_metal").box(0, 0, -0.22, 0.05, 2.1, 0, "M_trim").box(1.85, 0, -0.22, 1.9, 2.1, 0, "M_trim").socket("door", 0.95, 0, -0.11)
p = P("door_frame_bifold_120", "door_frame", snap="opening", door_kind="bifold")
p.box(0, 0, -0.22, 0.05, 2.15, 0, "M_trim").box(1.25, 0, -0.22, 1.3, 2.15, 0, "M_trim").box(0.05, 2.1, -0.22, 1.25, 2.15, 0, "M_trim").socket("door", 0.65, 0, -0.11)
p = P("door_frame_double_160", "door_frame", snap="opening", door_kind="double")
p.box(0, 0, -0.22, 0.05, 2.15, 0, "M_trim").box(1.65, 0, -0.22, 1.7, 2.15, 0, "M_trim").box(0.05, 2.1, -0.22, 1.65, 2.15, 0, "M_trim").socket("door", 0.85, 0, -0.11)
for kind, w, slot, extra in (("int", 0.9, "M_wood", None), ("entr", 0.9, "M_metal", "peephole"), ("fire", 0.9, "M_metal", "pushbar")):
    p = P("door_leaf_%s_%03d" % (kind, int(w * 100)), "door_leaf", snap="socket")
    p.box(0, 0, -0.045, w - 0.04, 2.08, 0, slot).socket("hinge", 0, 0, -0.0225).socket("handle", w - 0.12, 1.0, -0.045)
    if extra == "peephole":
        p.socket("peephole", (w - 0.04) / 2.0, 1.6, -0.045).socket("chain", w - 0.1, 1.45, -0.045)
    if extra == "pushbar":
        p.box(0.1, 1.0, -0.09, w - 0.14, 1.05, -0.045, "M_metal", collide=False)
P("door_leaf_slide_090", "door_leaf", snap="socket").box(0, 0, -0.04, 0.9, 2.08, 0, "M_wood").socket("handle", 0.8, 1.0, -0.04).socket("hinge", 0, 0, -0.02)
P("door_leaf_bifold_060", "door_leaf", snap="socket").box(0, 0, -0.03, 0.6, 2.06, 0, "M_wood").socket("handle", 0.55, 1.0, -0.03).socket("hinge", 0, 0, -0.015)
P("door_handle_lever", "door_hardware", snap="socket").box(0, 0, -0.07, 0.14, 0.02, 0, "M_metal", collide=False)
P("door_handle_knob", "door_hardware", snap="socket").box(0, 0, -0.06, 0.06, 0.06, 0, "M_metal", collide=False)
P("door_pushbar", "door_hardware", snap="socket").box(0, 0, -0.06, 0.7, 0.05, 0, "M_metal", collide=False)

# Windows / soft furnishing
P("win_fixed_150", "window", snap="opening").box(0, 0, -0.06, 1.2, 1.2, 0, "M_glass").box(0, 0, -0.08, 1.2, 0.05, 0, "M_metal")
P("win_casement_150", "window", snap="opening").box(0, 0, -0.06, 1.2, 1.2, 0, "M_glass").box(0.58, 0, -0.08, 0.62, 1.2, 0, "M_metal").socket("hinge", 0, 0, -0.03).socket("handle", 1.1, 0.6, -0.08)
P("win_corridor_end_180", "window", snap="opening").box(0, 0, -0.06, 1.8, 2.2, 0, "M_glass")
p = P("blinds_roller_150", "soft", snap="socket", animated="extent")
p.box(0, 2.5, -0.06, 1.44, 2.58, 0, "M_fabric", collide=False).box(0, 0, -0.04, 1.44, 2.5, -0.035, "M_fabric", collide=False)
p = P("blinds_venetian_150", "soft", snap="socket", animated="tilt")
p.box(0, 2.5, -0.06, 1.44, 2.58, 0, "M_metal", collide=False)
for k in range(10):
    p.box(0, k * 0.25, -0.05, 1.44, 0.02 + k * 0.25, -0.01, "M_metal", collide=False, lod1=(k % 3 == 0))
p = P("curtain_150", "soft", snap="socket", animated="open")
p.box(0, 0, -0.08, 0.75, 2.6, -0.04, "M_fabric", collide=False).box(0.75, 0, -0.08, 1.5, 2.6, -0.04, "M_fabric", collide=False).box(0, 2.6, -0.08, 1.5, 2.62, 0, "M_metal", collide=False)
P("curtain_rail_150", "soft", snap="socket").box(0, 0, -0.03, 1.5, 0.03, 0, "M_metal", collide=False)

# Trims
for w in (0.5, 1.0, 2.0):
    P("skirting_%03d" % int(w * 100), "trim", snap="wall").box(0, 0, -0.015, w, 0.1, 0, "M_trim", collide=False)
    if w > 0.5:
        P("cornice_%03d" % int(w * 100), "trim", snap="wall").box(0, 0, -0.06, w, 0.08, 0, "M_trim", collide=False)
P("skirting_corner_in", "trim", snap="wall").box(0, 0, -0.015, 0.015, 0.1, 0, "M_trim", collide=False)
P("skirting_corner_out", "trim", snap="wall").box(0, 0, -0.03, 0.03, 0.1, 0, "M_trim", collide=False)
P("cornice_corner_in", "trim", snap="wall").box(0, 0, -0.06, 0.06, 0.08, 0, "M_trim", collide=False)
P("cornice_corner_out", "trim", snap="wall").box(0, 0, -0.12, 0.12, 0.08, 0, "M_trim", collide=False)
for ow in (0.9, 1.2):
    p = P("casing_door_%03d" % int(ow * 100), "trim", snap="opening")
    p.box(0, 0, -0.02, 0.07, 2.17, 0, "M_trim", collide=False).box(ow + 0.07, 0, -0.02, ow + 0.14, 2.17, 0, "M_trim", collide=False).box(0, 2.1, -0.02, ow + 0.14, 2.17, 0, "M_trim", collide=False)
P("casing_win_150", "trim", snap="opening").box(0, 0, -0.02, 1.34, 0.07, 0, "M_trim", collide=False).box(0, 1.27, -0.02, 1.34, 1.34, 0, "M_trim", collide=False)

# Structure
P("column_sq_030", "structure", snap="grid").box(0, 0, -0.3, 0.3, H_EXT, 0, "M_concrete")
P("column_round_030", "structure", snap="grid").box(0.02, 0, -0.28, 0.28, H_EXT, -0.02, "M_concrete").box(0, 0, -0.3, 0.3, 0.05, 0, "M_concrete")
for w in (2.0, 4.0):
    P("beam_%03d" % int(w * 100), "structure", snap="grid").box(0, 0, -0.3, w, 0.4, 0, "M_concrete")
P("beam_down_stand_300", "structure", snap="grid").box(0, 0, -0.25, 3.0, 0.3, 0, "M_paint")

# Elevator
p = P("lift_door_pair", "lift", snap="opening", animated="slide")
p.box(0, 0, -0.04, 0.6, 2.1, 0, "M_metal").box(0.6, 0, -0.04, 1.2, 2.1, 0, "M_metal")
p = P("lift_frame", "lift", snap="opening")
p.box(0, 0, -0.25, 0.1, 2.3, 0, "M_metal").box(1.3, 0, -0.25, 1.4, 2.3, 0, "M_metal").box(0.1, 2.2, -0.25, 1.3, 2.3, 0, "M_metal").socket("door", 0.7, 0, -0.12)
P("lift_call_panel", "lift", snap="wall").box(0, 0, -0.02, 0.1, 0.2, 0, "M_metal", collide=False).socket("switch", 0.05, 0.1, -0.02)
p = P("lift_cab", "lift", snap="grid")
p.box(0, 0, -1.6, 1.6, 0.05, 0, "M_metal").box(0, 0, -1.6, 0.05, 2.3, 0, "M_metal").box(1.55, 0, -1.6, 1.6, 2.3, 0, "M_metal").box(0, 0, -1.6, 1.6, 2.3, -1.55, "M_metal").box(0, 2.25, -1.6, 1.6, 2.3, 0, "M_metal")
P("lift_shaft_300", "lift", snap="grid").box(0, 0, -2.2, 0.2, 3.0, 0, "M_concrete").box(2.0, 0, -2.2, 2.2, 3.0, 0, "M_concrete").box(0, 0, -2.2, 2.2, 3.0, -2.0, "M_concrete")

# Corridor
p = P("corridor_wall_door_recess_150", "corridor", snap="wall")
p.box(0, 0, -0.3, 0.25, H_INT, 0, "M_paint").box(1.25, 0, -0.3, 1.5, H_INT, 0, "M_paint").box(0.25, 2.15, -0.3, 1.25, H_INT, 0, "M_paint").socket("door", 0.75, 0, -0.15).socket("number_plate", 1.32, 1.6, -0.3)
P("corridor_number_plate", "corridor", snap="socket").box(0, 0, -0.01, 0.12, 0.08, 0, "M_metal", collide=False)
P("corridor_fire_extinguisher_cab", "corridor", snap="wall").box(0, 0, -0.18, 0.45, 0.7, 0, "M_metal").box(0.05, 0.05, -0.185, 0.4, 0.65, -0.18, "M_glass", collide=False)
P("corridor_light_ceiling", "corridor", snap="ceiling").box(0, 0, -0.2, 1.2, 0.05, 0, "M_emissive", collide=False).socket("light", 0.6, 0, -0.1)
P("corridor_exit_sign", "corridor", snap="wall").box(0, 0, -0.04, 0.35, 0.15, 0, "M_emissive", collide=False)
P("riser_access_panel", "corridor", snap="wall").box(0, 0, -0.02, 0.6, 1.2, 0, "M_metal", collide=False)

# Roof
for w in (1.0, 3.0):
    P("roof_parapet_%03d" % int(w * 100), "roof", snap="grid").box(0, 0, -0.25, w, 1.1, 0, "M_render").box(0, 1.1, -0.3, w, 1.15, 0.0, "M_metal", collide=False)
P("roof_parapet_corner", "roof", snap="grid").box(0, 0, -0.25, 0.25, 1.1, 0, "M_render")
p = P("roof_access_hut", "roof", snap="grid")
p.box(0, 0, -3.0, 0.2, 2.6, 0, "M_render").box(2.8, 0, -3.0, 3.0, 2.6, 0, "M_render").box(0, 0, -3.0, 3.0, 2.6, -2.8, "M_render")
p.box(0, 0, -0.2, 0.95, 2.6, 0, "M_render").box(1.95, 0, -0.2, 3.0, 2.6, 0, "M_render").box(0.95, 2.1, -0.2, 1.95, 2.6, 0, "M_render").box(0, 2.6, -3.0, 3.0, 2.8, 0, "M_concrete")
p.socket("door", 1.45, 0, -0.1)
P("roof_ac_pad", "roof", snap="grid").box(0, 0, -1.0, 2.0, 0.15, 0, "M_concrete")
P("roof_ac_unit", "roof", snap="grid").box(0, 0, -0.8, 1.2, 1.0, 0, "M_metal").box(0.2, 1.0, -0.6, 1.0, 1.05, -0.2, "M_plastic", collide=False)
P("roof_water_tank", "roof", snap="grid").box(0, 0, -2.0, 2.0, 0.5, 0, "M_metal").box(0.1, 0.5, -1.9, 1.9, 2.6, -0.1, "M_wood")
P("roof_vent", "roof", snap="grid").box(0, 0, -0.4, 0.4, 0.6, 0, "M_metal")
P("roof_antenna", "roof", snap="grid").box(0, 0, -0.05, 0.05, 4.0, 0, "M_metal", collide=False).box(-0.0, 3.0, -0.6, 0.05, 3.03, 0.0, "M_metal", collide=False)
p = P("roof_chimney_brick", "roof", snap="grid")
p.box(0, 0, -0.6, 0.9, 1.6, 0, "M_brick").box(0, 1.6, -0.6, 0.9, 1.7, 0, "M_stone")

# Façade bays (near city: 3 x 3 m per storey, front faces -Z)
for mat in ("brick", "stone", "concrete", "render"):
    slot = "M_" + mat
    for v, (ww, wh, sill) in zip("abc", ((1.2, 1.5, 0.9), (0.9, 1.8, 0.6), (2.0, 1.4, 0.9))):
        p = P("fac_%s_win_%s" % (mat, v), "facade", snap="facade")
        j = (3.0 - ww) / 2.0
        p.box(0, 0, -0.3, j, 3.0, 0, slot).box(3.0 - j, 0, -0.3, 3.0, 3.0, 0, slot).box(j, 0, -0.3, 3.0 - j, sill, 0, slot).box(j, sill + wh, -0.3, 3.0 - j, 3.0, 0, slot)
        p.box(j, sill, -0.22, 3.0 - j, sill + wh, -0.2, "M_window", collide=False).box(j, sill - 0.06, -0.3, 3.0 - j, sill, -0.26, "M_stone", collide=False)
        p.socket("interior_card", 1.5, sill, -0.1)
    P("fac_%s_blank" % mat, "facade", snap="facade").box(0, 0, -0.3, 3.0, 3.0, 0, slot)
    p = P("fac_%s_balcony" % mat, "facade", snap="facade")
    p.box(0, 0, -0.3, 0.6, 3.0, 0, slot).box(2.4, 0, -0.3, 3.0, 3.0, 0, slot).box(0.6, 2.4, -0.3, 2.4, 3.0, 0, slot)
    p.box(0.6, 0, -0.22, 2.4, 2.4, -0.2, "M_window", collide=False).box(0, 0, -1.3, 3.0, 0.2, -0.3, "M_concrete").box(0, 0.2, -1.3, 3.0, 1.1, -1.26, "M_metal", collide=False)
    P("fac_%s_corner" % mat, "facade", snap="facade").box(0, 0, -0.3, 0.3, 3.0, 0, slot)
for v in "abc":
    p = P("fac_shopfront_%s" % v, "facade", snap="facade")
    p.box(0, 0, -0.3, 0.3, 4.0, 0, "M_stone").box(2.7, 0, -0.3, 3.0, 4.0, 0, "M_stone").box(0.3, 3.0, -0.3, 2.7, 4.0, 0, "M_render")
    p.box(0.3, 0.4, -0.2, 2.7, 3.0, -0.18, "M_window", collide=False).box(0.3, 0, -0.3, 2.7, 0.4, 0, "M_stone").box(0.2, 3.0, -0.9, 2.8, 3.1, -0.3, "M_fabric" if v != "b" else "M_metal", collide=False)
for v in "ab":
    p = P("fac_entrance_%s" % v, "facade", snap="facade")
    p.box(0, 0, -0.3, 0.8, 4.0, 0, "M_stone").box(2.2, 0, -0.3, 3.0, 4.0, 0, "M_stone").box(0.8, 2.6, -0.3, 2.2, 4.0, 0, "M_stone").socket("door", 1.5, 0, -0.15)
P("fac_cornice_top", "facade", snap="facade").box(0, 0, -0.5, 3.0, 0.4, 0, "M_stone")
P("fac_rooftop_flat", "facade", snap="facade").box(0, 0, -3.0, 3.0, 0.2, 0, "M_concrete")
p = P("fac_rooftop_pitched_a", "facade", snap="facade")
for k in range(6):
    p.box(0, k * 0.25, -3.0 + k * 0.25, 3.0, (k + 1) * 0.25, -k * 0.25, "M_brick")
for v in "abcd":
    P("fac_interior_card_%s" % v, "facade", snap="socket").box(0, 0, -0.02, 1.2, 1.5, 0, "M_paint", collide=False)

# Furniture / fixtures (entities spawn at their sockets: kit.json "entity")
def furn(pid, w, h, d, slot, **meta):
    return P(pid, "furniture", snap="floor", **meta).box(0, 0, -d, w, h, 0, slot)
for w in (0.6,):
    furn("kit_base_060", w, 0.88, 0.6, "M_wood").box(0, 0.88, -0.62, w, 0.92, 0, "M_stone")
    furn("kit_base_drawers_060", w, 0.88, 0.6, "M_wood").box(0, 0.88, -0.62, w, 0.92, 0, "M_stone").socket("container", w / 2, 0, -0.3)
furn("kit_sink_090", 0.9, 0.88, 0.6, "M_wood").box(0, 0.88, -0.62, 0.9, 0.92, 0, "M_stone").box(0.15, 0.9, -0.5, 0.75, 0.93, -0.1, "M_metal", collide=False).socket("tap", 0.45, 0.92, -0.55).socket("container", 0.45, 0, -0.3)
furn("kit_hob_060", 0.6, 0.88, 0.6, "M_wood").box(0, 0.88, -0.62, 0.6, 0.92, 0, "M_stone").box(0.05, 0.92, -0.55, 0.55, 0.93, -0.05, "M_plastic", collide=False)
furn("kit_oven_060", 0.6, 0.88, 0.6, "M_metal").box(0, 0.88, -0.62, 0.6, 0.92, 0, "M_stone").box(0.05, 0.1, -0.61, 0.55, 0.7, -0.6, "M_glass", collide=False)
furn("kit_dw_060", 0.6, 0.88, 0.6, "M_wood").box(0, 0.88, -0.62, 0.6, 0.92, 0, "M_stone")
furn("kit_fridge_tall_060", 0.6, 2.0, 0.65, "M_metal").socket("container", 0.3, 0, -0.33)
P("kit_wall_060", "furniture", snap="wall").box(0, 0, -0.35, 0.6, 0.7, 0, "M_wood").socket("container", 0.3, 0, -0.18)
P("kit_wall_090", "furniture", snap="wall").box(0, 0, -0.35, 0.9, 0.7, 0, "M_wood").socket("container", 0.45, 0, -0.18)
p = P("kit_island_240", "furniture", snap="floor")
p.box(0.05, 0, -0.85, 2.35, 0.9, -0.05, "M_wood").box(0, 0.9, -0.9, 2.4, 0.94, 0, "M_stone").socket("container", 1.2, 0, -0.45)
furn("bath_vanity_090", 0.9, 0.85, 0.5, "M_wood").box(0.1, 0.85, -0.45, 0.8, 0.9, -0.05, "M_tile").socket("tap", 0.45, 0.9, -0.48).socket("container", 0.45, 0, -0.25)
furn("bath_wc", 0.4, 0.42, 0.65, "M_tile").box(0.02, 0.42, -0.2, 0.38, 0.8, 0, "M_tile")
furn("bath_shower_090", 0.9, 0.05, 0.9, "M_tile").box(0, 0, -0.02, 0.9, 2.0, 0, "M_glass").socket("tap", 0.45, 1.1, -0.85)
furn("bath_tub_170", 1.7, 0.55, 0.75, "M_tile").socket("tap", 0.1, 0.6, -0.37)
P("bath_mirror_cab", "furniture", snap="wall").box(0, 0, -0.14, 0.6, 0.7, 0, "M_wood").box(0, 0, -0.145, 0.6, 0.7, -0.14, "M_glass", collide=False).socket("container", 0.3, 0, -0.07)
furn("bed_double", 1.6, 0.5, 2.1, "M_fabric").box(0, 0, -2.1, 1.6, 1.0, -2.0, "M_wood")
furn("bed_single", 0.9, 0.45, 2.0, "M_fabric").box(0, 0, -2.0, 0.9, 0.8, -1.92, "M_wood")
p = furn("bed_loft_child", 0.95, 0.25, 2.0, "M_wood")
p.box(0, 1.2, -2.0, 0.95, 1.45, 0, "M_fabric").box(0, 0, -0.05, 0.05, 1.8, 0, "M_wood").box(0.9, 0, -0.05, 0.95, 1.8, 0, "M_wood").box(0, 0, -2.0, 0.05, 1.8, -1.95, "M_wood").box(0.9, 0, -2.0, 0.95, 1.8, -1.95, "M_wood")
furn("wardrobe_120", 1.2, 2.1, 0.6, "M_wood").socket("container", 0.6, 0, -0.3)
furn("dresser_100", 1.0, 0.85, 0.45, "M_wood").socket("container", 0.5, 0, -0.22)
furn("nightstand", 0.45, 0.55, 0.4, "M_wood").socket("container", 0.22, 0, -0.2).socket("prop", 0.22, 0.55, -0.2)
p = furn("sofa_l", 2.6, 0.45, 0.9, "M_fabric")
p.box(0, 0, -0.9, 2.6, 0.85, -0.7, "M_fabric")
furn("armchair", 0.85, 0.45, 0.85, "M_fabric").box(0, 0, -0.85, 0.85, 0.9, -0.68, "M_fabric")
furn("tv_credenza", 1.6, 0.5, 0.42, "M_wood").socket("prop", 0.8, 0.5, -0.2)
P("tv_55", "furniture", snap="socket").box(0, 0, -0.05, 1.23, 0.72, 0, "M_plastic").box(0.02, 0.02, -0.051, 1.21, 0.7, -0.05, "M_emissive", collide=False).socket("screen", 0.615, 0.36, -0.05)
p = furn("table_dining_4", 1.6, 0.04, 0.9, "M_wood")
p.boxes = []
p.box(0, 0.72, -0.9, 1.6, 0.76, 0, "M_wood")
for x, z in ((0.05, -0.1), (1.5, -0.1), (0.05, -0.85), (1.5, -0.85)):
    p.box(x, 0, z - 0.05, x + 0.05, 0.72, z, "M_wood")
p = furn("chair_dining", 0.45, 0.04, 0.45, "M_wood")
p.boxes = []
p.box(0, 0.45, -0.45, 0.45, 0.49, 0, "M_wood").box(0, 0, -0.45, 0.45, 0.95, -0.41, "M_wood").box(0.02, 0, -0.04, 0.06, 0.45, 0, "M_wood").box(0.39, 0, -0.04, 0.43, 0.45, 0, "M_wood")
furn("stool_bar", 0.4, 0.75, 0.4, "M_metal")
furn("desk_120", 1.2, 0.75, 0.6, "M_wood").socket("container", 0.9, 0, -0.3).socket("prop", 0.6, 0.75, -0.3)
furn("chair_office", 0.55, 0.5, 0.55, "M_fabric").box(0, 0, -0.55, 0.55, 1.0, -0.47, "M_fabric")
P("shelf_100", "furniture", snap="wall").box(0, 0, -0.3, 1.0, 1.8, 0, "M_wood").socket("container", 0.5, 0, -0.15)
furn("washer", 0.6, 0.85, 0.6, "M_metal").box(0.15, 0.25, -0.61, 0.45, 0.55, -0.6, "M_glass", collide=False)
furn("dryer", 0.6, 0.85, 0.6, "M_metal")
furn("water_heater", 0.5, 1.4, 0.5, "M_metal")
P("breaker_panel", "fixture", snap="wall").box(0, 0, -0.1, 0.4, 0.6, 0, "M_metal").socket("switch", 0.2, 0.3, -0.1)
furn("lamp_floor", 0.3, 1.6, 0.3, "M_metal").box(0, 1.4, -0.3, 0.3, 1.65, 0, "M_fabric", collide=False).socket("light", 0.15, 1.5, -0.15)
P("lamp_table", "fixture", snap="socket").box(0, 0, -0.2, 0.2, 0.45, 0, "M_fabric", collide=False).socket("light", 0.1, 0.35, -0.1)
P("light_downlight", "fixture", snap="ceiling").box(0, 0, -0.12, 0.12, 0.02, 0, "M_emissive", collide=False).socket("light", 0.06, 0, -0.06)
P("light_pendant", "fixture", snap="ceiling").box(0.08, 0, -0.18, 0.1, 0.6, -0.16, "M_metal", collide=False).box(0, 0, -0.26, 0.26, 0.2, 0, "M_metal", collide=False).socket("light", 0.13, 0.1, -0.13)
P("switch_plate", "fixture", snap="wall").box(0, 0, -0.012, 0.085, 0.085, 0, "M_plastic", collide=False).socket("switch", 0.0425, 0.0425, -0.012)
P("outlet_plate", "fixture", snap="wall").box(0, 0, -0.012, 0.085, 0.085, 0, "M_plastic", collide=False).socket("outlet", 0.0425, 0.0425, -0.012)
P("radiator_100", "fixture", snap="wall").box(0, 0.1, -0.1, 1.0, 0.7, 0, "M_metal").box(0.05, 0, -0.06, 0.1, 0.1, -0.02, "M_metal").box(0.9, 0, -0.06, 0.95, 0.1, -0.02, "M_metal")
P("coat_rack", "fixture", snap="wall").box(0, 0, -0.08, 0.8, 0.12, 0, "M_wood", collide=False)
furn("shoe_bench", 0.9, 0.45, 0.35, "M_wood")
furn("hall_cabinet", 0.9, 1.0, 0.45, "M_wood")
furn("toy_chest", 0.7, 0.45, 0.4, "M_plastic").socket("container", 0.35, 0, -0.2)
# Small props (no collision requirement)
for pid, w, h, d, slot in (("prop_bowl", 0.16, 0.06, 0.16, "M_plastic"), ("prop_mug", 0.08, 0.1, 0.08, "M_plastic"), ("prop_cereal_box", 0.2, 0.3, 0.07, "M_fabric"),
                           ("prop_photo_frame", 0.15, 0.2, 0.02, "M_wood"), ("prop_toy_blocks", 0.2, 0.08, 0.2, "M_plastic"), ("prop_dog_bowl", 0.22, 0.06, 0.22, "M_metal"),
                           ("prop_school_bag", 0.3, 0.4, 0.15, "M_fabric"), ("prop_calendar", 0.3, 0.4, 0.01, "M_paint"), ("prop_plant", 0.3, 0.9, 0.3, "M_fabric"),
                           ("prop_rug_200", 2.0, 0.01, 1.4, "M_carpet"), ("prop_laptop", 0.34, 0.02, 0.24, "M_plastic"), ("prop_phone", 0.075, 0.01, 0.15, "M_plastic"),
                           ("prop_trophy", 0.1, 0.25, 0.1, "M_metal"), ("prop_plush_dog", 0.25, 0.2, 0.15, "M_fabric")):
    P(pid, "prop", snap="surface").box(0, 0, -d, w, h, 0, slot, collide=False)
pieces = [p for p in pieces if p is not None]

# ------------------------------------------------------------------ manifest
CATEGORIES = {   # rules (src/assets/kit_piece.h KitRules): sockets, collision, triangle budget, footprint grid
    "wall": {"collision": True, "max_tris": 300, "grid": 0.05, "grid_height": 0.05},
    "wall_opening": {"collision": True, "sockets": ["open", "frame"], "max_tris": 300, "grid": 0.05, "grid_height": 0.05},
    "glass": {"collision": True, "max_tris": 200},
    "glass_trim": {"max_tris": 200},
    "balcony": {"collision": True, "max_tris": 200},
    "floor": {"collision": True, "max_tris": 200, "grid": 0.05},
    "floor_finish": {"max_tris": 100, "grid": 0.05},
    "floor_trim": {"max_tris": 100},
    "ceiling": {"max_tris": 200},
    "stair": {"collision": False, "max_tris": 600},
    "stair_rail": {"max_tris": 300},
    "door_frame": {"collision": True, "sockets": ["door"], "max_tris": 300},
    "door_leaf": {"collision": True, "sockets": ["hinge", "handle"], "max_tris": 300},
    "door_hardware": {"max_tris": 200},
    "window": {"collision": True, "max_tris": 300},
    "soft": {"max_tris": 600},
    "trim": {"max_tris": 200},
    "structure": {"collision": True, "max_tris": 200},
    "lift": {"max_tris": 600},
    "corridor": {"max_tris": 400},
    "roof": {"max_tris": 600},
    "facade": {"max_tris": 600},
    "furniture": {"collision": True, "max_tris": 800},
    "fixture": {"max_tris": 400},
    "prop": {"max_tris": 400},
}
ENTITIES = {   # piece -> the entity spawned at its socket (instances override params: name, loot, group, locked, on...)
    "door_frame_int_090": {"type": "door", "socket": "door", "width": 0.88, "height": 2.08},
    "door_frame_entr_090": {"type": "door", "socket": "door", "width": 0.88, "height": 2.08, "color": [0.55, 0.57, 0.6]},
    "door_frame_fire_090": {"type": "door", "socket": "door", "width": 0.88, "height": 2.08, "color": [0.7, 0.2, 0.15]},
    "door_frame_bifold_120": {"type": "door", "socket": "door", "width": 1.18, "height": 2.08},
    "door_frame_double_160": {"type": "door", "socket": "door", "width": 1.58, "height": 2.08},
    "door_frame_slide_090": {"type": "door", "socket": "door", "width": 1.38, "height": 2.08, "color": [0.75, 0.88, 0.95]},
    "glass_slider_frame_150": {"type": "door", "socket": "door", "width": 1.4, "height": 2.6, "color": [0.75, 0.88, 0.95]},
    "corridor_wall_door_recess_150": {"type": "door", "socket": "door", "width": 0.95, "height": 2.1, "locked": True},
    "light_downlight": {"type": "light", "socket": "light", "radius": 5, "intensity": 6, "offset": [0, -0.08, 0]},
    "light_pendant": {"type": "light", "socket": "light", "radius": 6, "intensity": 8, "offset": [0, -0.1, 0]},
    "corridor_light_ceiling": {"type": "light", "socket": "light", "radius": 7, "intensity": 6, "offset": [0, -0.08, 0], "color": [0.85, 0.9, 1.0]},
    "lamp_floor": {"type": "light", "socket": "light", "radius": 4, "intensity": 4},
    "lamp_table": {"type": "light", "socket": "light", "radius": 3, "intensity": 3},
    "switch_plate": {"type": "switch", "socket": "switch"},
}
for pid in ("kit_base_drawers_060", "kit_sink_090", "kit_fridge_tall_060", "kit_wall_060", "kit_wall_090", "kit_island_240", "bath_vanity_090", "bath_mirror_cab",
            "wardrobe_120", "dresser_100", "nightstand", "desk_120", "shelf_100", "toy_chest"):
    ENTITIES[pid] = {"type": "container", "socket": "container"}


def main():
    os.makedirs(os.path.join(OUT, "materials"), exist_ok=True)
    slot_colors = {"M_" + s: v["default"] for s, (surf, v) in MATERIALS.items()}
    for s, (surf, variants) in MATERIALS.items():
        doc = {"_doc": "Kit material set (docs/BUILDING_KIT.md 'Materials'): slot M_%s. Variants: [r, g, b, roughness, metallic(, emissive)]; "
                       "instances pick one per slot (\"mat\": {\"M_%s\": \"<variant>\"}) and may tint." % (s, s),
               "slot": "M_" + s, "surface": surf, "glass": s == "glass", "windows": s == "window", "variants": variants}
        with open(os.path.join(OUT, "materials", s + ".json"), "w", newline="") as f:
            json.dump(doc, f, indent=1)
    ids = set()
    manifest = {"_doc": "GENERATED by tools/kit_gen/kit_gen.py, then hand-edited entries are kept by id (docs/BUILDING_KIT.md). Pieces: category (rules), snap class, "
                        "entity spawned at a socket. tools/validate_kit checks every assets/kits/building/*.glb against these rules.",
                "units": "m", "up": "+Y", "front": "-Z", "pivot": "bottom-left-back", "texel_density_px_per_cm": 10.24, "categories": CATEGORIES, "pieces": {}}
    for p in pieces:
        assert p.id not in ids, p.id
        ids.add(p.id)
        front_container_sockets(p)
        write_glb(os.path.join(OUT, p.id + ".glb"), p, slot_colors)
        e = {"category": p.cat, "snap": p.meta.get("snap", "grid")}
        for k, v in p.meta.items():
            if k != "snap":
                e[k] = v
        if p.id in ENTITIES:
            e["entity"] = ENTITIES[p.id]
        manifest["pieces"][p.id] = e
    with open(os.path.join(OUT, "kit.json"), "w", newline="") as f:
        json.dump(manifest, f, indent=1)
    print("kit_gen: %d pieces, %d material sets -> %s" % (len(pieces), len(MATERIALS), os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
