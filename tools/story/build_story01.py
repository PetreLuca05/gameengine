"""Chapter 1 level: unit 10A, the 10th-floor corridor and the stair core assembled from the building kit
(docs/BUILDING_KIT.md) following the layout plan (docs/CHAPTER_01.md §4), plus the level around it.

Writes
  assets/kits/building/prefabs/apt_unit_corner_10a.json   the flat (walls, glass, doors, floors, furniture, lights)
  assets/kits/building/prefabs/corridor_10.json           corridor, neighbour doors, lift lobby
  assets/kits/building/prefabs/stair_core_dogleg.json     one storey of the stair core (flights, landings, walls)
  data/levels/story_01.json                               the prefabs placed, story entities, points / volumes /
                                                          interactables (the "story" block, src/game/game_story.c),
                                                          the tower shell and a placeholder city (brushes)
Coordinates: prefab-local = unit-local (x east, z south, floor y 0; the origin is the flat's north-west
interior corner); the level places the storey at y = 30. Plan lines are wall centrelines.
Run: python tools/story/build_story01.py   (after tools/kit_gen/kit_gen.py)
"""
import json, math, os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "kit_gen"))
import kit_gen as K  # noqa: E402  (the piece list: sizes, sockets)

FLOOR = 30.0
CEIL = 2.7
PREFABS = os.path.join(ROOT, "assets", "kits", "building", "prefabs")
LEVEL = os.path.join(ROOT, "data", "levels", "story_01.json")

SIZE = {}   # piece -> (w, h, d)
for p in K.pieces:
    mx = [max(b[1][i] for b in p.boxes) for i in range(3)]
    mn = [min(b[0][i] for b in p.boxes) for i in range(3)]
    SIZE[p.id] = (round(mx[0], 3), round(mx[1], 3), round(-mn[2], 3))


def r3(v):
    return round(v + 0.0, 4)


def inst(out, piece, x, y, z, yaw=0, **kw):
    assert piece in SIZE, piece
    e = {"piece": piece, "pos": [r3(x), r3(y), r3(z)]}
    if yaw:
        e["yaw"] = yaw
    e.update(kw)
    out.append(e)
    return e


YAW = {"N": 0, "S": 180, "E": -90, "W": 90}   # the piece's front (local -Z) faces this way


def place(out, piece, cx, cz, facing, y=0.0, **kw):
    """A piece centred on (cx, cz) (footprint centre) with its front facing N / S / E / W."""
    w, h, d = SIZE[piece]
    yaw = YAW[facing]
    lx, lz = w / 2.0, -d / 2.0                      # local footprint centre
    rx, rz = K_rot(yaw, lx, lz)
    return inst(out, piece, cx - rx, y, cz - rz, yaw, **kw)


def K_rot(yaw, x, z):
    a = math.radians(yaw)
    return x * math.cos(a) + z * math.sin(a), -x * math.sin(a) + z * math.cos(a)


def against(out, piece, wall_axis, wall_at, side, along, y=0.0, **kw):
    """A piece with its back on a wall face. wall_axis 'x' (wall runs along x at z = wall_at) or 'z';
    side: which side of the wall the piece stands (+1 = +z / +x side); along = its centre along the wall."""
    w, h, d = SIZE[piece]
    t = kw.pop("t", 0.2)
    face = wall_at + side * t / 2.0
    if wall_axis == "x":
        return place(out, piece, along, face + side * d / 2.0, "S" if side > 0 else "N", y, **kw)
    return place(out, piece, face + side * d / 2.0, along, "E" if side > 0 else "W", y, **kw)


SEGS = (4.0, 3.0, 2.0, 1.0, 0.5, 0.1, 0.05)


def split(length):
    out, left = [], round(length, 3)
    for s in SEGS:
        while left >= s - 1e-6:
            out.append(s)
            left = round(left - s, 3)
    assert left < 1e-6, (length, left)
    return out


def wall_run(out, a0, a1, at, axis, kind="int", openings=(), y=0.0, height=None):
    """A wall on a centreline from a0 to a1; openings: (centre, width 0.9 / 1.2 / 1.6, frame piece or None, entity params)."""
    t = 0.2 if kind == "int" else 0.3
    pos = a0
    for c, ow, frame, params in sorted(openings, key=lambda o: o[0]):
        wpiece = "wall_%s_door_%03d" % (kind, int(round(ow * 100)))
        ww = SIZE[wpiece][0]
        if c - ww / 2 > pos + 1e-6:
            _segments(out, pos, c - ww / 2, at, axis, kind, t, y)
        _seg(out, wpiece, c - ww / 2, at, axis, t, y)
        if frame:
            fw, fh, fd = SIZE[frame]
            if axis == "x":
                inst(out, frame, c - fw / 2, y, at + fd / 2, 0, **params)
            else:
                inst(out, frame, at - fd / 2, y, c - fw / 2, -90, **params)
        pos = c + ww / 2
    if a1 > pos + 1e-6:
        _segments(out, pos, a1, at, axis, kind, t, y)


def _segments(out, a0, a1, at, axis, kind, t, y):
    p = a0
    for s in split(a1 - a0):
        _seg(out, "wall_%s_%03d" % (kind, int(round(s * 100))), p, at, axis, t, y)
        p += s


def _seg(out, piece, a, at, axis, t, y):
    if axis == "x":
        inst(out, piece, a, y, at + t / 2.0)
    else:
        inst(out, piece, at - t / 2.0, y, a, -90)


def floor_rect(out, x0, x1, z0, z1, fin, y=0.0):
    """Floor finish tiles (2.0 / 1.0 / 0.5 / 0.2 / 0.1 m) covering a rectangle on the 0.1 m grid."""
    xs, zs = [], []
    p = x0
    for s in split_finish(x1 - x0):
        xs.append((p, s)); p += s
    p = z0
    for s in split_finish(z1 - z0):
        zs.append((p, s)); p += s
    for (x, w) in xs:
        for (z, d) in zs:
            piece = "floor_finish_%s_%03dx%03d" % (fin, int(round(w * 100)), int(round(d * 100)))
            inst(out, piece, x, y, z + d)


def split_finish(length):
    out, left = [], round(length, 3)
    for s in (2.0, 1.0, 0.5, 0.2, 0.1):
        while left >= s - 1e-6:
            out.append(s)
            left = round(left - s, 3)
    assert left < 1e-6, (length, left)
    return out


def slabs(out, x0, x1, z0, z1, y):
    """Structural slab (top at y) from 4 / 2 / 1 m tiles."""
    x = x0
    while x < x1 - 1e-6:
        w = 4.0 if x1 - x >= 4 else (2.0 if x1 - x >= 2 else 1.0)
        z = z0
        while z < z1 - 1e-6:
            d = min(w, 4.0 if z1 - z >= 4 else (2.0 if z1 - z >= 2 else 1.0))
            piece = {4.0: "floor_slab_400x400", 2.0: "floor_slab_200x200", 1.0: "floor_slab_100x100"}[min(w, d)]
            s = min(w, d)
            inst(out, piece, x, y - 0.3, z + s)
            z += s
        x += w


# ================================================================== unit 10A (prefab, floor y 0)
U = []
# Floor finishes (on the structural slab of the storey).
for (x0, x1, z0, z1, fin) in ((4.5, 12.0, 0.0, 6.0, "wood"), (0.0, 4.5, 0.0, 4.5, "wood"), (2.4, 4.5, 4.5, 7.0, "wood"),
                              (4.5, 6.0, 6.0, 12.0, "wood"), (6.0, 8.4, 6.0, 7.2, "wood"), (6.0, 8.4, 9.6, 12.0, "wood"),
                              (8.4, 12.0, 6.0, 12.0, "wood"), (3.0, 4.5, 10.5, 12.0, "wood"),
                              (0.0, 2.4, 4.5, 7.0, "tile"), (6.0, 8.4, 7.2, 9.6, "tile"), (3.0, 4.5, 7.0, 10.5, "tile")):
    floor_rect(U, x0, x1, z0, z1, fin)

# Party / corridor walls (0.3).
wall_run(U, 0.0, 7.0, 0.0, "z", "ext")
wall_run(U, 0.0, 3.0, 7.0, "x", "ext")
wall_run(U, 7.0, 12.0, 3.0, "z", "ext")
wall_run(U, 3.0, 12.0, 12.0, "x", "ext", openings=[(5.25, 0.9, "door_frame_entr_090", {"name": "door_10a"})])

# Interior partitions (0.2).
wall_run(U, 0.0, 4.5, 4.5, "z")
wall_run(U, 0.0, 4.5, 4.5, "x", openings=[(1.05, 0.9, "door_frame_int_090", {"name": "door_ensuite"}), (3.6, 0.9, None, {})])
wall_run(U, 4.5, 7.0, 2.4, "z")
wall_run(U, 4.5, 7.0, 4.5, "z", openings=[(6.3, 0.9, "door_frame_int_090", {"name": "door_wic"})])
wall_run(U, 0.0, 3.0, 7.0, "x")
wall_run(U, 3.0, 4.5, 7.0, "x")
wall_run(U, 7.0, 12.0, 4.5, "z", openings=[(8.35, 0.9, "door_frame_int_090", {"name": "door_utility"}), (11.25, 0.9, "door_frame_int_090", {"name": "door_coats"})])
wall_run(U, 3.0, 4.5, 10.5, "x")
wall_run(U, 6.0, 9.6, 6.0, "x")
wall_run(U, 9.6, 12.0, 6.0, "x")
wall_run(U, 7.2, 9.6, 6.0, "z", openings=[(8.4, 0.9, "door_frame_int_090", {"name": "door_bath"})])
wall_run(U, 10.6, 12.0, 6.0, "z")
wall_run(U, 6.0, 8.4, 7.2, "x")
wall_run(U, 6.0, 8.4, 9.6, "x")
wall_run(U, 6.0, 8.4, 10.6, "x", openings=[(7.2, 0.9, "door_frame_int_090", {"name": "door_storage"})])
wall_run(U, 6.0, 9.0, 8.4, "z", openings=[(6.6, 0.9, "door_frame_int_090", {"name": "door_teen"})])
wall_run(U, 9.0, 12.0, 8.4, "z", openings=[(10.1, 0.9, "door_frame_int_090", {"name": "door_child"})])
wall_run(U, 8.4, 12.0, 9.0, "x")

# Full-height glass: north (z = 0, front faces north) and east (x = 12, front faces east), 1.5 m modules.
for k in range(8):
    x = k * 1.5
    if k:
        inst(U, "glass_mullion", x - 0.03, 0, 0.075)
    if k == 7:
        inst(U, "glass_slider_frame_150", x, 0, 0.075, name="balcony_door")
    else:
        inst(U, "glass_panel_150", x, 0, 0.015)
for k in range(8):
    z = k * 1.5
    if k:
        inst(U, "glass_mullion", 12.0 - 0.075, 0, z - 0.03, -90)
    inst(U, "glass_panel_150", 12.0 - 0.015, 0, z, -90)
inst(U, "glass_mullion_corner", 11.925, 0, 0.075)
inst(U, "glass_mullion", -0.03, 0, 0.075)

# Balcony: slabs (top at floor level), glass balustrade on the outer edges.
for k in range(5):
    inst(U, "balcony_slab_150x120", 4.5 + k * 1.5, -0.3, 0.0)
inst(U, "balcony_slab_corner_120", 12.0, -0.3, 0.0)
for k in range(4):
    inst(U, "balcony_slab_150x120", 12.0, -0.3, k * 1.5, -90)
for k in range(5):
    inst(U, "balustrade_glass_150", 4.5 + k * 1.5, 0, -1.17)
inst(U, "balustrade_glass_120", 12.0, 0, -1.17)
for k in range(4):
    inst(U, "balustrade_glass_150", 13.17, 0, k * 1.5, -90)
inst(U, "balustrade_glass_120", 13.17, 0, -1.2, -90)

# Furniture (names / loot drive the chapter: assets/story/chapters/01_tenth_floor.json).
# Master
place(U, "bed_double", 2.2, 2.05, "S")
place(U, "nightstand", 1.0, 1.0, "S")
place(U, "nightstand", 3.4, 1.0, "S")
against(U, "dresser_100", "x", 4.5, -1, 2.2, name="master_dresser", loot=[["painkillers", 1]])
place(U, "lamp_table", 1.0, 1.0, "S", y=0.55, group=10, on=False, name="l_master_lamp")
# En-suite / WIC
against(U, "bath_shower_090", "z", 0.0, +1, 6.2, t=0.3)
against(U, "bath_wc", "x", 7.0, -1, 1.6)
against(U, "bath_vanity_090", "z", 2.4, -1, 5.4)
against(U, "bath_mirror_cab", "z", 2.4, -1, 5.4, y=1.3, name="ensuite_cabinet", loot=[["medkit", 1], ["bandage", 1]])
against(U, "shelf_100", "z", 2.4, +1, 5.5, name="wic_shelf", loot=[["cloth", 2]])
against(U, "wardrobe_120", "x", 7.0, -1, 3.5)
# Kitchen run along z = 6.0 (front north), fridge at the west end
kx = 4.6
for piece, name, loot in (("kit_fridge_tall_060", "fridge", [["water_bottle", 2], ["energy_bar", 2]]), ("kit_base_drawers_060", "kitchen_drawer_1", [["knife", 1]]),
                          ("kit_base_060", None, None), ("kit_sink_090", "kitchen_sink", None), ("kit_dw_060", None, None),
                          ("kit_base_drawers_060", "kitchen_cabinet_1", [["canned_food", 2]]), ("kit_hob_060", None, None), ("kit_base_060", None, None)):
    w = SIZE[piece][0]
    kw = {}
    if name:
        kw["name"] = name
    if loot:
        kw["loot"] = loot
    against(U, piece, "x", 6.0, -1, kx + w / 2, **kw)
    kx += w
against(U, "kit_wall_090", "x", 6.0, -1, 6.7, y=1.5, name="kitchen_cabinet_2", loot=[["water_bottle", 1]])
against(U, "kit_wall_060", "x", 6.0, -1, 8.4, y=1.5)
place(U, "kit_island_240", 7.4, 4.55, "N", name="island_drawer", loot=[["duct_tape", 1]])
for x in (6.7, 8.1):
    place(U, "stool_bar", x, 3.85, "N")
# Dining
place(U, "table_dining_4", 6.2, 1.5, "N")
for (x, z, f) in ((5.85, 0.8, "S"), (6.55, 0.8, "S"), (5.85, 2.2, "N"), (6.55, 2.2, "N")):
    place(U, "chair_dining", x, z, f)
# Living: L-sofa facing the NE corner, armchair, TV on the south partition, reading lamp
place(U, "sofa_l", 9.9, 2.95, "N")
place(U, "armchair", 8.3, 1.2, "E")
against(U, "tv_credenza", "x", 6.0, -1, 11.0, t=0.2)
against(U, "tv_55", "x", 6.0, -1, 11.0, y=0.65)
place(U, "lamp_floor", 7.9, 0.4, "S", group=12, on=False, name="l_reading")
place(U, "prop_rug_200", 9.9, 1.8, "N")
place(U, "prop_plant", 11.6, 0.4, "S")
place(U, "prop_dog_bowl", 8.0, 5.4, "N")
# Hall / coats / utility
against(U, "shoe_bench", "z", 4.5, +1, 9.6)
against(U, "coat_rack", "z", 4.5, +1, 10.0, y=1.6)
against(U, "wardrobe_120", "z", 4.5, -1, 11.3, name="coat_closet", loot=[["backpack", 1]])
against(U, "washer", "z", 3.0, +1, 7.4, t=0.3)
against(U, "dryer", "z", 3.0, +1, 7.4, y=0.85, t=0.3)
against(U, "water_heater", "x", 10.5, -1, 3.75)
against(U, "breaker_panel", "z", 3.0, +1, 9.95, y=1.1, t=0.3)
against(U, "shelf_100", "z", 4.5, -1, 8.0, name="utility_toolbox", loot=[["duct_tape", 1], ["battery", 2], ["flare", 1]])
# Study nook
against(U, "desk_120", "x", 6.0, +1, 7.2)
place(U, "chair_office", 7.2, 6.75, "N")
# Family bath
against(U, "bath_tub_170", "x", 9.6, -1, 7.05)
against(U, "bath_vanity_090", "z", 8.4, -1, 7.8)
against(U, "bath_mirror_cab", "z", 8.4, -1, 7.8, y=1.3, name="bath_cabinet", loot=[["medkit", 1], ["bandage", 2]])
against(U, "bath_wc", "x", 7.2, +1, 6.5)
# Storage
against(U, "shelf_100", "x", 12.0, -1, 7.2, t=0.3, name="storage_shelf", loot=[["canned_food", 3], ["water_bottle", 2], ["flare", 1]])
# Teen (Jules)
place(U, "bed_single", 11.45, 7.6, "W")
against(U, "wardrobe_120", "z", 8.4, +1, 7.4, name="teen_wardrobe", loot=[["energy_bar", 1]])
against(U, "desk_120", "x", 9.0, -1, 9.4)
place(U, "prop_trophy", 10.6, 8.55, "N", y=0.75)
# Child (Mia)
place(U, "bed_loft_child", 11.45, 10.5, "W")
place(U, "toy_chest", 9.4, 11.6, "N", name="child_toybox", loot=[["flare", 1]])
place(U, "prop_toy_blocks", 9.8, 10.3, "N")
place(U, "prop_plush_dog", 10.2, 9.6, "N")

# Lights (ceiling downlights, group per room = power group bits) and switches by the doors.
LIGHTS = [("l_master", 2.2, 2.2, 10), ("l_ensuite", 1.2, 5.8, 11), ("l_wic", 3.4, 5.8, 11), ("l_dining", 6.0, 1.6, 12), ("l_kitchen", 7.2, 4.6, 12),
          ("l_living", 9.8, 2.8, 12), ("l_hall_1", 5.25, 7.5, 13), ("l_hall_2", 5.25, 10.5, 13), ("l_utility", 3.75, 8.8, 13),
          ("l_bath", 7.2, 8.4, 14), ("l_study", 7.2, 6.6, 14), ("l_teen", 10.2, 7.5, 15), ("l_child", 10.2, 10.5, 15)]
for name, x, z, g in LIGHTS:
    place(U, "light_downlight", x, z, "N", y=CEIL - 0.02, name=name, group=g, on=False)
for name, wall_axis, at, side, along, g in (("sw_master", "z", 4.5, -1, 3.0, 10), ("sw_living", "x", 6.0, -1, 4.9, 12), ("sw_hall", "z", 4.5, +1, 11.5, 13),
                                            ("sw_teen", "z", 8.4, +1, 6.1, 15), ("sw_child", "z", 8.4, +1, 9.6, 15), ("sw_bath", "z", 6.0, +1, 7.7, 14)):
    against(U, "switch_plate", wall_axis, at, side, along, y=1.1, name=name, group=g)
U = [e for e in U if e]

# ================================================================== corridor (prefab, floor y 0)
C = []
wall_run(C, -8.5, 3.0, 12.15, "x", "ext", openings=[(1.05, 0.9, "door_frame_entr_090", {"name": "door_10b", "locked": True})])
wall_run(C, -8.5, 12.0, 14.25, "x", "ext", openings=[(-4.05, 0.9, "door_frame_entr_090", {"name": "door_10d", "locked": True}),
                                                     (3.0, 1.6, None, {}), (8.95, 0.9, "door_frame_entr_090", {"name": "door_10c", "locked": True})])
floor_rect(C, -8.5, 12.0, 12.3, 14.1, "tile")
for k, x in enumerate((-6.0, -1.0, 4.0, 9.0)):
    place(C, "corridor_light_ceiling", x, 13.2, "N", y=CEIL - 0.05, name="l_corridor_%d" % k, group=20)
inst(C, "win_corridor_end_180", 12.0, 0.3, 12.3, -90)
against(C, "corridor_exit_sign", "z", -8.5, +1, 13.2, y=2.25, t=0.3)
against(C, "corridor_fire_extinguisher_cab", "x", 14.25, -1, -1.5, y=0.9, t=0.3)
against(C, "riser_access_panel", "x", 12.15, +1, 2.5, y=0.3, t=0.3)
# Lift lobby (x 0..6, z 14.4..17.5): side walls, back wall with two lift doors.
wall_run(C, 14.4, 17.5, 0.0, "z", "ext")
wall_run(C, 14.4, 17.5, 6.0, "z", "ext")
wall_run(C, 0.0, 6.0, 17.65, "x", "ext", openings=[(1.6, 1.2, None, {}), (4.4, 1.2, None, {})])
for x in (1.6, 4.4):
    inst(C, "lift_frame", x - 0.7, 0, 17.65 + 0.125)
    inst(C, "lift_door_pair", x - 0.6, 0, 17.65 + 0.02)
against(C, "lift_call_panel", "x", 17.65, -1, 3.0, y=1.1, t=0.3)
floor_rect(C, 0.0, 6.0, 14.4, 17.5, "tile")

# ================================================================== stair core (prefab: one storey, landing at y 0)
S = []
CX0, CX1 = -12.0, -8.5
wall_run(S, 6.0, 20.0, CX1, "z", "ext", openings=[(13.2, 0.9, "door_frame_fire_090", {"name": "fire_door"})])
wall_run(S, CX0, CX1, 5.85, "x", "ext")
wall_run(S, CX0, CX1, 20.15, "x", "ext")
wall_run(S, 6.0, 20.0, CX0 - 0.15, "z", "ext")
slabs(S, CX0, CX1 - 0.5, 11.0, 15.0, 0.0)          # floor landing (z 11..15), x -12..-9
# Landing strip x -9..-8.5 (0.5 wide): 0.5 x 0.5 isn't in the kit: use 1 x 0.5 slabs turned 90 degrees.
for z in (11.0, 12.0, 13.0, 14.0):
    inst(S, "floor_slab_100x050", CX1, -0.3, z, -90)
slabs(S, CX0, CX1 - 0.5, 6.0, 8.0, 1.5)             # half landing (z 6..8.5) at +1.5
for x in (CX0, CX0 + 1.0, CX0 + 2.0):
    inst(S, "floor_slab_100x050", x, 1.2, 8.5)
for z in (6.0, 7.0):
    inst(S, "floor_slab_100x050", CX1, 1.2, z, -90)
inst(S, "stair_flight_150x150", CX0, 0.0, 11.0)            # west flight: up north from the landing to the half landing
inst(S, "stair_flight_150x150", CX1, 1.5, 8.5, 180)        # east flight: up south from the half landing to the next floor
inst(S, "stair_rail_flight", CX0 + 1.5, 0.0, 11.0)
inst(S, "stair_newel", CX0 + 1.5, 0.0, 11.1)
S = [e for e in S if e]

for name, data, doc in (("apt_unit_corner_10a", U, "Unit 10A, the chapter 1 flat (docs/CHAPTER_01.md §4). Unit-local: origin = NW interior corner, floor y 0."),
                        ("corridor_10", C, "The 10th-floor corridor, neighbour doors, lift lobby (docs/CHAPTER_01.md §4)."),
                        ("stair_core_dogleg", S, "One storey of the dog-leg stair core: landing at y 0, half landing at 1.5, the fire door to the corridor.")):
    os.makedirs(PREFABS, exist_ok=True)
    with open(os.path.join(PREFABS, name + ".json"), "w", newline="") as f:
        json.dump({"prefab": name, "_doc": doc + " GENERATED by tools/story/build_story01.py.", "pieces": data}, f, indent=1)

# ================================================================== the level
brushes, entities, points, volumes, interact = [], [], {}, [], []


def box(x0, x1, z0, z1, y0, y1, color, material="plaster", **kw):
    b = {"type": "box", "pos": [round((x0 + x1) / 2, 3), round(y0, 3), round((z0 + z1) / 2, 3)],
         "size": [round(x1 - x0, 3), round(y1 - y0, 3), round(z1 - z0, 3)], "color": color, "material": material, "grid": False}
    b.update(kw)
    brushes.append(b)


def ent(type_, pos, **kw):
    e = {"type": type_, "pos": [round(v, 3) for v in pos]}
    e.update(kw)
    entities.append(e)
    return e


def point(name, x, z, yaw=0.0, y=FLOOR):
    points[name] = {"pos": [x, y, z], "yaw": yaw}


def volume(name, x0, x1, z0, z1, y0=FLOOR - 0.2, y1=FLOOR + CEIL):
    volumes.append({"name": name, "min": [x0, y0, z0], "max": [x1, y1, z1]})


def story_obj(name, x, y, z, kind, prompt, size=(0.12, 0.01, 0.09), color=(0.95, 0.95, 0.85), **kw):
    ent("marker", (x, FLOOR + y, z), name=name, shape="box", half=list(size), color=list(color), solid=False)
    spec = {"name": name, "kind": kind, "prompt": prompt}
    spec.update(kw)
    interact.append(spec)


EXT = [0.62, 0.6, 0.58]
kit = []
prefabs = [{"prefab": "apt_unit_corner_10a", "pos": [0, FLOOR, 0]}, {"prefab": "corridor_10", "pos": [0, FLOOR, 0]}]
for k, y in enumerate((FLOOR - 3.0, FLOOR, FLOOR + 3.0)):
    prefabs.append({"prefab": "stair_core_dogleg", "pos": [0, y, 0], "prefix": "f%d_" % (9 + k), "override": {"fire_door": {"locked": y != FLOOR}}})
# Storey slabs (top at the floor; ceiling slab above) over the flat, corridor, lobby.
slabs(kit, 0.0, 12.0, 0.0, 12.0, FLOOR)
slabs(kit, -8.0, 12.0, 12.0, 18.0, FLOOR)
slabs(kit, 0.0, 12.0, 0.0, 12.0, FLOOR + 3.0)
slabs(kit, -8.0, 12.0, 12.0, 18.0, FLOOR + 3.0)
# The rest of the storey (neighbours, the notch) as closed blocks; the tower below / above.
box(-8.5, 0.0, 0.0, 12.0, FLOOR, FLOOR + 3.0, [0.6, 0.58, 0.55])
box(0.0, 3.0, 7.15, 11.85, FLOOR, FLOOR + 3.0, [0.6, 0.58, 0.55])
box(-8.5, -0.15, 14.4, 24.0, FLOOR, FLOOR + 3.0, [0.6, 0.58, 0.55])
box(6.15, 12.0, 17.5, 24.0, FLOOR, FLOOR + 3.0, [0.6, 0.58, 0.55])
box(0.0, 6.0, 17.8, 24.0, FLOOR, FLOOR + 3.0, [0.6, 0.58, 0.55])
box(-12.3, 12.15, 0.0, 24.3, 0.0, FLOOR - 0.3, EXT, "concrete", collide=False)
box(-12.3, 12.15, 0.0, 24.3, FLOOR + 3.3, 48.0, EXT, "concrete", collide=False)
box(-12.0, -8.5, 6.0, 6.3, FLOOR + 1.5, FLOOR + 3.0, [0.35, 0.3, 0.25], "metal_thin", label="CLOSED")   # roof: chapter 2

# Story entities (not kit pieces): the bat, story interactables, the barricade cabinet.
ent("pickup", (10.2, FLOOR + 0.8, 9.25), item="bat", name="pickup_bat")
story_obj("phone_nightstand", 1.0, 0.56, 1.0, "use", "Check phone", size=(0.04, 0.005, 0.075), color=(0.05, 0.05, 0.06), signal="phone_messages_read", show="msg.01_family")
story_obj("note_dana", 6.2, 0.93, 5.55, "read", "Read note", show="doc.note_dana")
story_obj("note_jules", 4.9, 1.4, 5.34, "read", "Read note", size=(0.08, 0.1, 0.005), show="doc.note_jules")
story_obj("note_mia_slip", 9.8, 0.76, 11.4, "read", "Read permission slip", show="doc.note_mia_slip")
story_obj("calendar", 4.62, 1.5, 3.5, "read", "Read calendar", size=(0.005, 0.2, 0.15), color=(0.95, 0.9, 0.8), show="doc.calendar")
story_obj("rota", 5.2, 1.3, 5.34, "read", "Read rota", size=(0.08, 0.1, 0.005), show="doc.rota")
story_obj("safe_code", 7.4, 0.76, 6.35, "read", "Read sticky note", size=(0.04, 0.005, 0.04), color=(1.0, 0.9, 0.3), show="doc.safe_code")
story_obj("answering_machine", 8.8, 0.95, 4.3, "use", "Play messages", size=(0.08, 0.04, 0.06), color=(0.1, 0.1, 0.1),
          signal="voicemail_dana_played", audio="vo.voicemail_dana", subtitle="vo.voicemail_dana", read="voicemail_dana")
story_obj("jules_laptop", 10.2, 0.76, 8.55, "use", "Look at the laptop", size=(0.17, 0.01, 0.12), color=(0.2, 0.2, 0.22), show="doc.jules_laptop", read="jules_laptop")
story_obj("dog_collar", 12.6, 0.02, 3.5, "pickup", "Pick up collar", size=(0.06, 0.01, 0.06), color=(0.8, 0.15, 0.1), show="doc.dog_collar", read="dog_collar")
story_obj("breaker_panel", 3.25, 1.4, 9.95, "hold", "Hold to reset the breaker", size=(0.03, 0.25, 0.18), color=(0.6, 0.62, 0.65), hold=2.5)
story_obj("peephole_10a", 5.25, 1.6, 11.8, "use", "Look through the peephole", size=(0.02, 0.02, 0.01), color=(0.7, 0.7, 0.6), peek="door_10a")
story_obj("hall_cabinet", 4.85, 0.5, 9.1, "hold", "Hold to push the cabinet against the door", size=(0.05, 0.4, 0.4), color=(0.45, 0.35, 0.25),
          hold=3.0, move={"entity": "hall_cabinet_barricade", "to": "barricade_pos"})
story_obj("pack_bag", 5.4, 0.45, 9.6, "use", "Pack and go", size=(0.15, 0.2, 0.1), color=(0.2, 0.3, 0.55), signal="pack_confirmed")
story_obj("lift_call", 3.0, 1.2, 17.45, "use", "Call the lift", size=(0.06, 0.1, 0.01), color=(0.3, 0.3, 0.3), signal="lift_dead", subtitle="sub.lift_dead")
ent("prop", (4.85, FLOOR, 8.7), name="hall_cabinet_barricade", shape="box", size=[0.45, 1.0, 0.9], color=[0.45, 0.35, 0.25], mass=60)

point("spawn_bed", 2.2, 3.4, 0)
point("spawn_hall", 5.25, 9.0, 0)
point("spawn_teen", 9.5, 7.0, 90)
point("spawn_10b", -4.0, 6.0, 0)
point("barricade_pos", 5.25, 11.3, 0)
point("zombie_10d", -4.05, 15.2, 0)
point("corridor_mid", 2.0, 13.2, 0)
point("heli_crash_anchor", 12.0 + math.sin(math.radians(47)) * 1750, -math.cos(math.radians(47)) * 1750, 0, y=60.0)
point("stair_up", -10.2, 7.0, 0, y=FLOOR + 3.0)
point("tv_living", 11.0, 5.8, 0, y=FLOOR + 0.9)
point("door_10a", 5.25, 12.0, 0, y=FLOOR + 1.6)

volume("vol_glass_rooms", 4.5, 12.0, 0.0, 6.0)
volume("vol_glass_rooms", 0.0, 4.5, 0.0, 4.5)
volume("vol_glass_rooms", 8.4, 12.0, 6.0, 12.0)
volume("vol_glass_rooms", 4.5, 13.2, -1.2, 0.0)
volume("vol_glass_rooms", 12.0, 13.2, 0.0, 6.0)
volume("vol_balcony", 4.5, 13.2, -1.2, 0.0)
volume("vol_balcony", 12.0, 13.2, 0.0, 6.0)
volume("vol_teen_room", 8.4, 12.0, 6.0, 9.0)
volume("vol_unit_10a", 0.0, 12.0, 0.0, 12.0)
volume("vol_corridor_10", -8.5, 12.0, 12.3, 14.1)
volume("vol_stair_9_5", CX0, CX1, 6.0, 8.5, FLOOR - 1.7, FLOOR - 0.2)

# Placeholder city (vista greybox until milestone 7).
rng = random.Random(1010)
CXc, CZc = 0.0, 12.0


def river_r(b):
    return 470 + 0.02 * b * b + (0 if b < 50 else (b - 50) * 25)


def in_river(x, z):
    b = math.degrees(math.atan2(x - CXc, -(z - CZc)))
    if b < -95 or b > 90:
        return False
    return abs(math.hypot(x - CXc, z - CZc) - river_r(b)) < 60


city = 0
while city < 260:
    d = rng.uniform(60, 650)
    a = rng.uniform(-180, 180)
    x, z = CXc + math.sin(math.radians(a)) * d, CZc - math.cos(math.radians(a)) * d
    if in_river(x, z) or (abs(x) < 40 and abs(z - 12) < 40):
        continue
    w_, dd = rng.uniform(16, 40), rng.uniform(16, 40)
    h = rng.uniform(6, 16) if d < 150 else (rng.uniform(9, 26) if d < 350 else rng.uniform(12, 40))
    col = rng.choice([[0.55, 0.38, 0.3], [0.6, 0.5, 0.42], [0.5, 0.48, 0.45], [0.65, 0.6, 0.52], [0.45, 0.32, 0.28]])
    box(x - w_ / 2, x + w_ / 2, z - dd / 2, z + dd / 2, 0.0, h, col, "brick", collide=False, yaw=rng.choice([0, 0, 0, 15, -10]))
    city += 1
for k in range(48):
    b = -90 + k * 3.8
    rd = river_r(b)
    x, z = CXc + math.sin(math.radians(b)) * rd, CZc - math.cos(math.radians(b)) * rd
    box(x - 45, x + 45, z - 45, z + 45, 0.0, 0.06, [0.28, 0.38, 0.45], "water", collide=False, yaw=b)
hx, hz = math.sin(math.radians(82)) * 360, 12 - math.cos(math.radians(82)) * 360
box(hx - 15, hx + 15, hz - 15, hz + 15, 0.0, 95.0, [0.5, 0.5, 0.55], "concrete", collide=False, label="Halden Tower")
sx, sz = math.sin(math.radians(28)) * 1400, 12 - math.cos(math.radians(28)) * 1400
brushes.append({"type": "box", "pos": [round(sx, 1), 0, round(sz, 1)], "size": [40, 220, 40], "color": [0.55, 0.65, 0.75], "material": "glass",
                "grid": False, "collide": False, "metallic": 0.6, "roughness": 0.2})

kit = [e for e in kit if e]
# Interior volumes (docs/STORY.md "Rendering"): sky light inside falls off away from the glass.
Y0, Y1 = FLOOR - 0.1, FLOOR + CEIL + 0.05
interiors = [
    {"name": "ldk", "min": [4.5, Y0, 0.0], "max": [12.0, Y1, 6.0], "sky": 0.22, "glass": ["-z", "+x"], "falloff": 3.0},
    {"name": "master", "min": [0.0, Y0, 0.0], "max": [4.5, Y1, 4.5], "sky": 0.18, "glass": ["-z"], "falloff": 2.5},
    {"name": "teen", "min": [8.4, Y0, 6.0], "max": [12.0, Y1, 9.0], "sky": 0.18, "glass": ["+x"], "falloff": 2.0},
    {"name": "child", "min": [8.4, Y0, 9.0], "max": [12.0, Y1, 12.0], "sky": 0.18, "glass": ["+x"], "falloff": 2.0},
    {"name": "hall", "min": [4.5, Y0, 6.0], "max": [6.0, Y1, 12.0], "sky": 0.06},
    {"name": "middle", "min": [6.0, Y0, 6.0], "max": [8.4, Y1, 12.0], "sky": 0.05},
    {"name": "west", "min": [0.0, Y0, 4.5], "max": [4.5, Y1, 12.0], "sky": 0.05},
    {"name": "corridor", "min": [-8.5, Y0, 12.3], "max": [12.2, Y1, 14.1], "sky": 0.04, "glass": ["+x"], "falloff": 1.5},
    {"name": "lobby", "min": [0.0, Y0, 14.1], "max": [6.0, Y1, 17.6], "sky": 0.03},
    {"name": "stair", "min": [-12.0, FLOOR - 3.4, 6.0], "max": [-8.5, FLOOR + 6.0, 20.0], "sky": 0.03},
]
level = {
    "name": "Story 01: 10th Floor",
    "_comment": "GENERATED by tools/story/build_story01.py (docs/CHAPTER_01.md): the storey from the building kit (\"prefabs\", \"kit\"; "
                "assets/kits/building/prefabs), story entities, the story block, a placeholder city. Edit the script, not this file. "
                "x east, z south, yaw 0 looks -Z (north), floor top y = 30.",
    "env": {"sun_dir": [0.6, 0.35, -0.5], "sun_color": [1.0, 0.85, 0.7], "sun_intensity": 2.0, "sky": [0.5, 0.58, 0.7],
            "ground": [0.3, 0.29, 0.27], "fog": [0.62, 0.66, 0.72], "fog_density": 0.0004,
            "sky_zenith": [0.24, 0.42, 0.72], "sky_horizon": [0.78, 0.8, 0.84], "clouds": 0.3},
    "room": {"size": [60, 10, 60], "walls": False, "ceiling": False, "floor_size": [3000, 0, 3000], "floor_material": "asphalt",
             "floor_color": [0.32, 0.33, 0.34]},
    "spawns": [{"pos": points["spawn_bed"]["pos"], "yaw": 0}, {"pos": points["spawn_hall"]["pos"], "yaw": 0},
               {"pos": points["spawn_teen"]["pos"], "yaw": 90}, {"pos": [5.25, FLOOR, 13.2], "yaw": 0}],
    "interiors": interiors,
    "prefabs": prefabs,
    "kit": kit,
    "brushes": brushes,
    "entities": entities,
    "story": {"_doc": "src/game/game_story.c: points (spawn / action targets), volumes (boxes; same name = union), "
                      "interactables (markers made usable: read / use / hold / pickup), power groups (light groups).",
              "points": points, "volumes": volumes, "interact": interact,
              "power": {"apt_10a": [10, 11, 12, 13, 14, 15], "building": [20, 21]}},
}
with open(LEVEL, "w", newline="") as f:
    json.dump(level, f, indent=1)
print("story_01: unit %d pieces, corridor %d, stair core %d, level kit %d, %d brushes, %d entities, %d interactables"
      % (len(U), len(C), len(S), len(kit), len(brushes), len(entities), len(interact)))
