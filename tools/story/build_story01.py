"""Chapter 1 greybox: writes data/levels/story_01.json from the layout plan (docs/CHAPTER_01.md §4).

Brushes, doors, glass, rooms, loot, lights / switches / power groups, the stair core, the corridor,
story interactables, points and volumes (the level's "story" block, read by src/story/story_run.c),
and a placeholder city (boxes) until the building kit (docs/BUILDING_KIT.md) and the vista
(docs/STORY.md §6) replace them. Coordinates: unit-local = world (x east, z south, floor top y = 30).
Run: python tools/story/build_story01.py
"""
import json, math, os, random

FLOOR = 30.0
CEIL = 2.7
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "data", "levels", "story_01.json")

brushes, entities, points, volumes, interact = [], [], {}, [], []

WALL = [0.86, 0.84, 0.8]
EXT = [0.62, 0.6, 0.58]
FLOOR_WOOD = [0.55, 0.42, 0.3]
TILE = [0.82, 0.84, 0.85]
DOOR_COL = [0.92, 0.9, 0.86]


def box(x0, x1, z0, z1, y0, y1, color, material="plaster", **kw):
    b = {"type": "box", "pos": [round((x0 + x1) / 2, 3), round(y0, 3), round((z0 + z1) / 2, 3)],
         "size": [round(x1 - x0, 3), round(y1 - y0, 3), round(z1 - z0, 3)], "color": color, "material": material, "grid": False}
    b.update(kw)
    brushes.append(b)


def wall(a0, a1, at, axis, thick=0.2, openings=(), color=WALL, height=CEIL, y0=FLOOR, material="plaster"):
    """A wall run on a grid line. axis 'x': runs along x from a0 to a1 at z = at; 'z': along z at x = at.
    openings: (centre, width, door_name or None, locked) along the run."""
    cuts = sorted(openings)
    pos = a0
    for c, wdt, name, locked in cuts:
        seg0, seg1 = c - wdt / 2 - 0.1, c + wdt / 2 + 0.1
        if seg0 > pos + 1e-3:
            _run(pos, seg0, at, axis, thick, color, height, y0, material)
        d = {"type": "doorway", "pos": _p(c, at, axis, y0), "yaw": 0 if axis == "x" else 90, "length": round(seg1 - seg0, 3),
             "height": height, "thickness": thick, "door_width": wdt, "door_height": 2.1, "color": color, "material": material,
             "grid": False, "label": " "}
        if name:
            d["door"] = {"name": name, "locked": locked, "color": DOOR_COL}
        brushes.append(d)
        pos = seg1
    if a1 > pos + 1e-3:
        _run(pos, a1, at, axis, thick, color, height, y0, material)


def _p(along, at, axis, y):
    return [round(along, 3), y, round(at, 3)] if axis == "x" else [round(at, 3), y, round(along, 3)]


def _run(a0, a1, at, axis, thick, color, height, y0, material):
    if axis == "x":
        box(a0, a1, at - thick / 2, at + thick / 2, y0, y0 + height, color, material)
    else:
        box(at - thick / 2, at + thick / 2, a0, a1, y0, y0 + height, color, material)


def glass_run(a0, a1, at, axis, skip=()):
    """Full-height glass: a pane per 1.5 m module + mullions; modules in skip stay open (sliding door)."""
    n = int(round((a1 - a0) / 1.5))
    for k in range(n + 1):
        m = a0 + k * 1.5
        if axis == "x":
            box(m - 0.03, m + 0.03, at - 0.075, at + 0.075, FLOOR - 0.3, FLOOR + 3.0, [0.16, 0.16, 0.18], "metal_thin")
        else:
            box(at - 0.075, at + 0.075, m - 0.03, m + 0.03, FLOOR - 0.3, FLOOR + 3.0, [0.16, 0.16, 0.18], "metal_thin")
    for k in range(n):
        if k in skip:
            continue
        m0, m1 = a0 + k * 1.5 + 0.03, a0 + (k + 1) * 1.5 - 0.03
        if axis == "x":
            box(m0, m1, at - 0.01, at + 0.01, FLOOR, FLOOR + CEIL, [0.7, 0.85, 0.92], "glass", glass=True)
        else:
            box(at - 0.01, at + 0.01, m0, m1, FLOOR, FLOOR + CEIL, [0.7, 0.85, 0.92], "glass", glass=True)


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
    """A story interactable: a small non-solid marker the runtime makes usable (src/story/story_run.c)."""
    ent("marker", (x, FLOOR + y, z), name=name, shape="box", half=list(size), color=list(color), solid=False)
    spec = {"name": name, "kind": kind, "prompt": prompt}
    spec.update(kw)
    interact.append(spec)


# ------------------------------------------------------------------ structure
# Floor slab under unit + corridor + core; the unit's floor finishes; ceiling.
box(-12.3, 13.5, -1.2, 24.3, FLOOR - 0.3, FLOOR, [0.5, 0.5, 0.5], "concrete")
box(4.5, 12.0, 0.0, 6.0, FLOOR, FLOOR + 0.01, FLOOR_WOOD, "wood", collide=False)
box(0.0, 4.5, 0.0, 4.5, FLOOR, FLOOR + 0.01, FLOOR_WOOD, "wood", collide=False)
box(8.4, 12.0, 6.0, 12.0, FLOOR, FLOOR + 0.01, FLOOR_WOOD, "wood", collide=False)
box(0.0, 2.4, 4.5, 7.0, FLOOR, FLOOR + 0.01, TILE, "tile", collide=False)
box(6.0, 8.4, 7.2, 9.6, FLOOR, FLOOR + 0.01, TILE, "tile", collide=False)
box(-12.3, 13.5, -1.2, 24.3, FLOOR + CEIL, FLOOR + 3.0, [0.9, 0.9, 0.9], "concrete")   # ceiling / slab above
# Tower below (reads as the building from the balcony) and above (to the roof at 48 m).
box(-12.3, 12.15, 0.0, 24.3, 0.0, FLOOR - 0.3, EXT, "concrete", collide=False)
box(-12.3, 12.15, 0.0, 24.3, FLOOR + 3.0, 48.0, EXT, "concrete", collide=False)

# Glass walls (north z = 0, east x = 12). North module 7 (x 10.5..12) is the sliding door.
glass_run(0.0, 12.0, 0.0, "x", skip=(7,))
glass_run(0.0, 12.0, 12.0, "z")
brushes.append({"type": "doorway", "pos": [11.25, FLOOR, 0.0], "yaw": 0, "length": 1.5, "height": CEIL, "thickness": 0.06,
                "door_width": 1.4, "door_height": 2.6, "color": [0.7, 0.85, 0.92], "material": "glass", "glass": True, "grid": False,
                "label": " ", "door": {"name": "balcony_door", "color": [0.75, 0.88, 0.95]}})
# Balcony: slab, glass balustrade, posts.
box(4.5, 13.2, -1.2, 0.0, FLOOR - 0.3, FLOOR, [0.55, 0.55, 0.55], "concrete")
box(12.0, 13.2, 0.0, 6.0, FLOOR - 0.3, FLOOR, [0.55, 0.55, 0.55], "concrete")
box(4.5, 13.2, -1.22, -1.18, FLOOR, FLOOR + 1.1, [0.7, 0.85, 0.92], "glass", glass=True)
box(13.18, 13.22, -1.2, 6.0, FLOOR, FLOOR + 1.1, [0.7, 0.85, 0.92], "glass", glass=True)
box(4.45, 4.5, -1.2, 0.0, FLOOR, FLOOR + 1.1, [0.7, 0.85, 0.92], "glass", glass=True)
box(12.0, 13.2, 5.98, 6.02, FLOOR, FLOOR + 1.1, [0.7, 0.85, 0.92], "glass", glass=True)
box(4.5, 13.2, -1.24, -1.16, FLOOR + 1.08, FLOOR + 1.12, [0.3, 0.3, 0.32], "metal_thin")
box(13.16, 13.24, -1.2, 6.0, FLOOR + 1.08, FLOOR + 1.12, [0.3, 0.3, 0.32], "metal_thin")

# Party / corridor walls (0.3): west of the master + en-suite, the notch, the corridor side.
wall(0.0, 7.0, 0.0, "z", 0.3, color=EXT)
wall(0.0, 3.0, 7.0, "x", 0.3, color=EXT)
wall(7.0, 12.0, 3.0, "z", 0.3, color=EXT)
wall(3.0, 12.0, 12.0, "x", 0.3, color=EXT, openings=[(5.25, 0.9, "door_10a", False)])

# Interior partitions (0.2).
wall(0.0, 4.5, 4.5, "z", openings=[])                                       # master | LDK (x = 4.5, z 0..4.5)
wall(0.0, 4.5, 4.5, "x", openings=[(1.05, 0.8, "door_ensuite", False), (3.75, 0.9, None, False)])  # master | en-suite + WIC
wall(4.5, 7.0, 2.4, "z", openings=[])                                       # en-suite | WIC
wall(4.5, 6.0, 4.5, "z")                                                    # WIC | hall (upper)
wall(6.0, 7.0, 4.5, "z", openings=[(6.45, 0.8, "door_wic", False)])         # WIC | hall
wall(0.0, 2.4, 7.0, "x")                                                    # en-suite south (party side)
wall(2.4, 4.5, 7.0, "x")
wall(7.0, 12.0, 4.5, "z", openings=[(8.45, 0.8, "door_utility", False), (11.25, 0.9, None, False)])  # hall west: utility, coats
wall(3.0, 4.5, 10.5, "x")                                                   # utility | coats
wall(6.0, 9.6, 6.0, "x")                                                    # kitchen back / study + bath
wall(9.6, 12.0, 6.0, "x")                                                   # living | teen
wall(6.0, 7.2, 6.0, "z")                                                    # hall | study (open nook: short return)
wall(7.2, 9.6, 6.0, "z", openings=[(8.2, 0.8, "door_bath", False)])         # hall | bath
wall(10.6, 12.0, 6.0, "z")                                                  # hall | storage
wall(6.0, 8.4, 9.6, "x")                                                    # bath | side hall
wall(6.0, 8.4, 10.6, "x", openings=[(7.0, 0.8, "door_storage", False)])     # side hall | storage
wall(6.0, 9.0, 8.4, "z", openings=[(6.6, 0.9, "door_teen", False)])         # study/bath | teen
wall(9.0, 12.0, 8.4, "z", openings=[(10.1, 0.9, "door_child", False)])      # side hall/storage | child
wall(8.4, 12.0, 9.0, "x")                                                   # teen | child

# Corridor (z 12.3..14.1, x -8.5..12) and its far side; neighbour doors; end window.
wall(-8.5, 3.0, 12.15, "x", 0.3, color=EXT, openings=[(1.05, 0.9, "door_10b", True)])
wall(-8.5, 12.0, 14.25, "x", 0.3, color=EXT, openings=[(-4.05, 0.9, "door_10d", True), (8.95, 0.9, "door_10c", True)])
box(-8.5, 0.0, 0.0, 12.0, FLOOR, FLOOR + CEIL, [0.6, 0.58, 0.55], "plaster")          # 10B (closed volume)
box(0.0, 3.0, 7.15, 12.0, FLOOR, FLOOR + CEIL, [0.6, 0.58, 0.55], "plaster")
box(-8.5, 0.0, 14.4, 24.0, FLOOR, FLOOR + CEIL, [0.6, 0.58, 0.55], "plaster")         # 10D
box(6.0, 12.0, 17.5, 24.0, FLOOR, FLOOR + CEIL, [0.6, 0.58, 0.55], "plaster")         # 10C
box(12.0, 12.15, 12.3, 14.1, FLOOR, FLOOR + CEIL, [0.7, 0.85, 0.92], "glass", glass=True)   # corridor end window
# Elevator lobby (x 0..6, z 14.4..17.5): two dead lifts.
box(0.0, 6.0, 17.5, 17.8, FLOOR, FLOOR + CEIL, EXT, "concrete")
box(1.0, 2.2, 17.42, 17.5, FLOOR, FLOOR + 2.1, [0.55, 0.57, 0.6], "metal_thin", label="1")
box(3.8, 5.0, 17.42, 17.5, FLOOR, FLOOR + 2.1, [0.55, 0.57, 0.6], "metal_thin", label="2")
box(-0.15, 0.0, 14.4, 17.5, FLOOR, FLOOR + CEIL, EXT, "concrete")
box(6.0, 6.15, 14.4, 17.5, FLOOR, FLOOR + CEIL, EXT, "concrete")

# Stair core (x -12..-8.5, z 6..20): dog-leg, floors 9 / 10 / 11. The fire door is on the corridor's west end.
CORE_X0, CORE_X1 = -12.0, -8.5
wall(6.0, 20.0, CORE_X1, "z", 0.3, color=EXT, height=9.0, y0=FLOOR - 3.0,
     openings=[(13.2, 0.9, "fire_door_10", False)])
box(CORE_X0 - 0.3, CORE_X0, 6.0, 20.0, FLOOR - 3.3, FLOOR + 6.0, EXT, "concrete")
box(CORE_X0, CORE_X1, 5.7, 6.0, FLOOR - 3.3, FLOOR + 6.0, EXT, "concrete")
box(CORE_X0, CORE_X1, 20.0, 20.3, FLOOR - 3.3, FLOOR + 6.0, EXT, "concrete")
for base in (FLOOR - 3.0, FLOOR, FLOOR + 3.0):           # landings at floors 9, 10, 11 (z 11..15)
    box(CORE_X0, CORE_X1, 11.0, 15.0, base - 0.3, base, [0.5, 0.5, 0.5], "concrete")
for base in (FLOOR - 1.5, FLOOR + 1.5):                   # half landings (z 6..8.5)
    box(CORE_X0, CORE_X1, 6.0, 8.5, base - 0.3, base, [0.5, 0.5, 0.5], "concrete")
for y_low in (FLOOR - 3.0, FLOOR):                         # west flight: rises north from a floor to the half landing
    brushes.append({"type": "stairs", "pos": [-11.125, y_low, 11.0], "yaw": 0, "rise": 1.5 / 9, "run": 0.28, "count": 9, "width": 1.75,
                    "color": [0.62, 0.62, 0.62], "material": "concrete", "grid": False})
for y_low in (FLOOR - 1.5, FLOOR + 1.5):                   # east flight: rises south from the half landing to the next floor
    brushes.append({"type": "stairs", "pos": [-9.375, y_low, 8.5], "yaw": 180, "rise": 1.5 / 9, "run": 0.28, "count": 9, "width": 1.75,
                    "color": [0.62, 0.62, 0.62], "material": "concrete", "grid": False})
box(CORE_X0, CORE_X1, 6.0, 6.3, FLOOR + 1.5, FLOOR + 3.0, [0.35, 0.3, 0.25], "metal_thin", label="CLOSED")  # chained gate (roof is ch. 2)
box(-10.27, -10.23, 8.5, 11.0, FLOOR - 3.0, FLOOR + 4.5, [0.25, 0.25, 0.27], "metal_thin", collide=False)  # stair eye rail
box(CORE_X1 - 0.02, CORE_X1, 12.6, 13.8, FLOOR - 3.0, FLOOR - 0.9, [0.6, 0.15, 0.1], "metal_thin", collide=False, label="9")

# ------------------------------------------------------------------ rooms: props, loot, lights
def prop_box(x0, x1, z0, z1, h, color, material="wood", y=0.0, collide=True):
    box(x0, x1, z0, z1, FLOOR + y, FLOOR + y + h, color, material, collide=collide)

# Master
prop_box(1.2, 3.2, 1.0, 3.1, 0.5, [0.75, 0.75, 0.8], "cloth")
prop_box(0.6, 1.1, 1.0, 1.5, 0.55, [0.45, 0.33, 0.24]); prop_box(3.3, 3.8, 1.0, 1.5, 0.55, [0.45, 0.33, 0.24])
ent("container", (2.2, FLOOR, 4.2), yaw=180, name="master_dresser", loot=[["painkillers", 1]])
# En-suite / WIC
prop_box(0.05, 1.1, 5.8, 6.95, 0.05, TILE, "tile")
ent("container", (1.9, FLOOR, 6.75), yaw=180, name="ensuite_cabinet", loot=[["medkit", 1], ["bandage", 1]])
ent("container", (3.5, FLOOR, 6.75), yaw=180, name="wic_shelf", loot=[["cloth", 2]])
# Kitchen / dining / living
prop_box(4.6, 9.6, 5.35, 5.95, 0.9, [0.85, 0.85, 0.82], "wood")
prop_box(6.2, 8.6, 4.1, 5.0, 0.9, [0.5, 0.4, 0.32], "wood")
prop_box(5.4, 7.0, 1.0, 2.0, 0.75, [0.55, 0.42, 0.3], "wood")
prop_box(8.6, 11.2, 2.4, 3.3, 0.45, [0.35, 0.37, 0.42], "cloth"); prop_box(10.4, 11.2, 0.9, 2.4, 0.45, [0.35, 0.37, 0.42], "cloth")
prop_box(10.2, 11.8, 5.6, 5.95, 0.5, [0.3, 0.25, 0.2], "wood")
prop_box(10.4, 11.6, 5.9, 5.95, 0.7, [0.05, 0.05, 0.06], "default", y=0.6)  # TV
ent("container", (5.2, FLOOR, 5.45), yaw=180, name="kitchen_drawer_1", loot=[["knife", 1]])
ent("container", (6.6, FLOOR, 5.45), yaw=180, name="kitchen_cabinet_1", loot=[["canned_food", 2]])
ent("container", (8.0, FLOOR, 5.45), yaw=180, name="kitchen_cabinet_2", loot=[["water_bottle", 1]])
ent("container", (9.2, FLOOR, 5.45), yaw=180, name="fridge", loot=[["water_bottle", 2], ["energy_bar", 2]])
ent("container", (7.4, FLOOR, 4.0), yaw=0, name="island_drawer", loot=[["duct_tape", 1]])
# Utility / coats / storage
ent("container", (3.75, FLOOR, 8.0), yaw=90, name="utility_toolbox", loot=[["duct_tape", 1], ["battery", 2], ["flare", 1]])
ent("container", (3.75, FLOOR, 11.5), yaw=90, name="coat_closet", loot=[["backpack", 1]])
ent("container", (7.2, FLOOR, 11.7), yaw=180, name="storage_shelf", loot=[["canned_food", 3], ["water_bottle", 2], ["flare", 1]])
# Family bath
prop_box(6.05, 7.7, 8.85, 9.55, 0.55, [0.95, 0.95, 0.95], "tile")
ent("container", (8.0, FLOOR, 7.4), yaw=0, name="bath_cabinet", loot=[["medkit", 1], ["bandage", 2]])
# Teen / child
prop_box(10.0, 11.9, 6.2, 7.6, 0.45, [0.3, 0.45, 0.35], "cloth")
prop_box(10.4, 11.9, 9.2, 11.1, 1.4, [0.85, 0.6, 0.7], "wood")
ent("pickup", (9.2, FLOOR + 0.05, 8.6), item="bat", name="pickup_bat")
ent("container", (8.8, FLOOR, 6.6), yaw=90, name="teen_wardrobe", loot=[["energy_bar", 1]])
ent("container", (9.0, FLOOR, 11.6), yaw=180, name="child_toybox", loot=[["flare", 1]])

# Lights per room (group = power group bit), switches by the doors.
LIGHTS = [  # name, x, z, group, intensity
    ("l_master", 2.2, 2.2, 10, 10), ("l_ensuite", 1.2, 5.8, 11, 6), ("l_wic", 3.4, 5.8, 11, 5),
    ("l_dining", 6.0, 1.6, 12, 9), ("l_kitchen", 7.2, 4.6, 12, 10), ("l_living", 9.8, 2.8, 12, 10),
    ("l_hall_1", 5.25, 7.5, 13, 6), ("l_hall_2", 5.25, 10.5, 13, 6), ("l_utility", 3.75, 8.8, 13, 5),
    ("l_bath", 7.2, 8.4, 14, 6), ("l_study", 7.2, 6.6, 14, 5), ("l_teen", 10.2, 7.5, 15, 8), ("l_child", 10.2, 10.5, 15, 8),
]
for name, x, z, g, inten in LIGHTS:
    ent("light", (x, FLOOR + 2.55, z), name=name, group=g, radius=6, intensity=inten, color=[1.0, 0.92, 0.8], on=False)
for name, x, z, g, yaw in (("sw_master", 4.3, 3.3, 10, 90), ("sw_living", 6.3, 5.9, 12, 0), ("sw_hall", 4.7, 11.4, 13, 90),
                           ("sw_teen", 8.6, 7.4, 15, 270), ("sw_child", 8.6, 9.3, 15, 270), ("sw_bath", 6.2, 7.6, 14, 90)):
    ent("switch", (x, FLOOR + 1.1, z), name=name, group=g, yaw=yaw)
# Corridor + stair lights (building circuit, emergency lighting after the cut).
for k, x in enumerate((-6.0, -1.0, 4.0, 9.0)):
    ent("light", (x, FLOOR + 2.55, 13.2), name="l_corridor_%d" % k, group=20, radius=7, intensity=6, color=[0.85, 0.9, 1.0])
ent("light", (-10.2, FLOOR + 2.5, 13.0), name="l_stair_10", group=21, radius=8, intensity=5, color=[0.9, 0.95, 1.0])
ent("light", (-10.2, FLOOR - 0.5, 13.0), name="l_stair_9", group=21, radius=8, intensity=3, color=[1.0, 0.4, 0.3], flicker=0.6)

# ------------------------------------------------------------------ story: interactables, points, volumes
story_obj("phone_nightstand", 0.85, 0.56, 1.25, "use", "Check phone", size=(0.04, 0.005, 0.075), color=(0.05, 0.05, 0.06),
          signal="phone_messages_read", show="msg.01_family")
story_obj("note_dana", 6.0, 0.91, 5.5, "read", "Read note", show="doc.note_dana")
story_obj("note_jules", 9.25, 1.4, 5.33, "read", "Read note", size=(0.08, 0.1, 0.005), show="doc.note_jules")
story_obj("note_mia_slip", 11.2, 0.8, 11.8, "read", "Read permission slip", show="doc.note_mia_slip")
story_obj("calendar", 4.65, 1.5, 3.5, "read", "Read calendar", size=(0.005, 0.2, 0.15), color=(0.95, 0.9, 0.8), show="doc.calendar")
story_obj("rota", 8.8, 1.3, 5.33, "read", "Read rota", size=(0.08, 0.1, 0.005), show="doc.rota")
story_obj("safe_code", 6.8, 0.8, 6.1, "read", "Read sticky note", size=(0.04, 0.005, 0.04), color=(1.0, 0.9, 0.3), show="doc.safe_code")
story_obj("answering_machine", 8.4, 0.92, 4.4, "use", "Play messages", size=(0.08, 0.04, 0.06), color=(0.1, 0.1, 0.1),
          signal="voicemail_dana_played", audio="vo.voicemail_dana", subtitle="vo.voicemail_dana", read="voicemail_dana")
story_obj("jules_laptop", 9.4, 0.76, 7.0, "use", "Look at the laptop", size=(0.17, 0.01, 0.12), color=(0.2, 0.2, 0.22),
          show="doc.jules_laptop", read="jules_laptop")
story_obj("dog_collar", 12.6, 0.02, 3.5, "pickup", "Pick up collar", size=(0.06, 0.01, 0.06), color=(0.8, 0.15, 0.1),
          show="doc.dog_collar", read="dog_collar")
story_obj("breaker_panel", 3.05, 1.4, 9.95, "hold", "Hold to reset the breaker", size=(0.04, 0.3, 0.22), color=(0.6, 0.62, 0.65), hold=2.5)
story_obj("peephole_10a", 5.25, 1.6, 11.88, "use", "Look through the peephole", size=(0.02, 0.02, 0.01), color=(0.7, 0.7, 0.6),
          peek="door_10a")
story_obj("hall_cabinet", 4.7, 0.5, 9.4, "hold", "Hold to push the cabinet against the door", size=(0.2, 0.5, 0.45), color=(0.45, 0.35, 0.25),
          hold=3.0, move={"entity": "hall_cabinet_barricade", "to": "barricade_pos"})
story_obj("pack_bag", 5.25, 0.45, 11.0, "use", "Pack and go", size=(0.15, 0.2, 0.1), color=(0.2, 0.3, 0.55), signal="pack_confirmed")
story_obj("lift_call", 2.9, 1.2, 17.38, "use", "Call the lift", size=(0.06, 0.1, 0.01), color=(0.3, 0.3, 0.3), signal="lift_dead",
          subtitle="sub.lift_dead")
ent("prop", (4.75, FLOOR, 9.4), name="hall_cabinet_barricade", shape="box", size=[0.45, 1.0, 0.9], color=[0.45, 0.35, 0.25], mass=60)

point("spawn_bed", 2.2, 2.0, 0)
point("spawn_hall", 5.25, 9.0, 0)
point("spawn_teen", 9.5, 7.5, 90)
point("spawn_10b", -4.0, 6.0, 0)
point("barricade_pos", 5.25, 11.4, 0)
point("zombie_10d", -4.05, 15.2, 0)
point("corridor_mid", 2.0, 13.2, 0)
point("heli_crash_anchor", 12.0 + math.sin(math.radians(47)) * 1750, -math.cos(math.radians(47)) * 1750, 0, y=60.0)
point("stair_up", -10.2, 7.0, 0, y=FLOOR + 3.0)
point("tv_living", 11.0, 5.8, 0, y=FLOOR + 0.9)
point("door_10a", 5.25, 12.0, 0, y=FLOOR + 1.6)

volume("vol_glass_rooms", 4.5, 12.0, 0.0, 6.0)           # LDK
volume("vol_glass_rooms", 0.0, 4.5, 0.0, 4.5)            # master
volume("vol_glass_rooms", 8.4, 12.0, 6.0, 12.0)          # teen + child
volume("vol_glass_rooms", 4.5, 13.2, -1.2, 0.0)          # balcony north
volume("vol_glass_rooms", 12.0, 13.2, 0.0, 6.0)          # balcony east
volume("vol_balcony", 4.5, 13.2, -1.2, 0.0)
volume("vol_balcony", 12.0, 13.2, 0.0, 6.0)
volume("vol_teen_room", 8.4, 12.0, 6.0, 9.0)
volume("vol_unit_10a", 0.0, 12.0, 0.0, 12.0)
volume("vol_corridor_10", -8.5, 12.0, 12.3, 14.1)
volume("vol_stair_9_5", CORE_X0, CORE_X1, 6.0, 8.5, FLOOR - 1.7, FLOOR - 0.2)

# ------------------------------------------------------------------ placeholder city (vista greybox)
rng = random.Random(1010)
CX, CZ = 0.0, 12.0
def in_river(x, z):
    # River centre ~470 m north, bending north-east (docs/story/ch01/vista_site_plan.svg); 120 m wide.
    b = math.degrees(math.atan2(x - CX, -(z - CZ)))
    if b < -95 or b > 90:
        return False
    rd = 470 + 0.02 * b * b + (0 if b < 50 else (b - 50) * 25)
    d = math.hypot(x - CX, z - CZ)
    return abs(d - rd) < 60
city = 0
while city < 260:
    d = rng.uniform(60, 650)
    a = rng.uniform(-180, 180)
    x, z = CX + math.sin(math.radians(a)) * d, CZ - math.cos(math.radians(a)) * d
    if in_river(x, z) or (abs(x) < 40 and abs(z - 12) < 40):
        continue
    w_, dd = rng.uniform(16, 40), rng.uniform(16, 40)
    h = rng.uniform(6, 16) if d < 150 else (rng.uniform(9, 26) if d < 350 else rng.uniform(12, 40))
    col = rng.choice([[0.55, 0.38, 0.3], [0.6, 0.5, 0.42], [0.5, 0.48, 0.45], [0.65, 0.6, 0.52], [0.45, 0.32, 0.28]])
    box(x - w_ / 2, x + w_ / 2, z - dd / 2, z + dd / 2, 0.0, h, col, "brick", collide=False, yaw=rng.choice([0, 0, 0, 15, -10]))
    city += 1
for k in range(48):  # river band
    b = -90 + k * 3.8
    rd = 470 + 0.02 * b * b + (0 if b < 50 else (b - 50) * 25)
    x, z = CX + math.sin(math.radians(b)) * rd, CZ - math.cos(math.radians(b)) * rd
    box(x - 45, x + 45, z - 45, z + 45, 0.0, 0.06, [0.28, 0.38, 0.45], "water", collide=False, yaw=b)
# Landmarks (placeholders): Halden Tower 82° 360 m; the spire (2.2 km) lives in the vista layer later.
hx, hz = math.sin(math.radians(82)) * 360, 12 - math.cos(math.radians(82)) * 360
box(hx - 15, hx + 15, hz - 15, hz + 15, 0.0, 95.0, [0.5, 0.5, 0.55], "concrete", collide=False, label="Halden Tower")
sx, sz = math.sin(math.radians(28)) * 1400, 12 - math.cos(math.radians(28)) * 1400   # pulled inside zfar until the vista layer
brushes.append({"type": "box", "pos": [round(sx, 1), 0, round(sz, 1)], "size": [40, 220, 40], "color": [0.55, 0.65, 0.75], "material": "glass",
                "grid": False, "collide": False, "metallic": 0.6, "roughness": 0.2})

level = {
    "name": "Story 01: 10th Floor",
    "_comment": "GENERATED by tools/story/build_story01.py (greybox; docs/CHAPTER_01.md). Do not edit by hand: change the script. "
                "Unit-local = world coordinates: x east, z south, yaw 0 looks -Z (north), floor top y = 30.",
    "env": {"sun_dir": [0.6, 0.35, -0.5], "sun_color": [1.0, 0.85, 0.7], "sun_intensity": 2.0, "sky": [0.5, 0.58, 0.7],
            "ground": [0.3, 0.29, 0.27], "fog": [0.62, 0.66, 0.72], "fog_density": 0.0004,
            "sky_zenith": [0.24, 0.42, 0.72], "sky_horizon": [0.78, 0.8, 0.84], "clouds": 0.3},
    "room": {"size": [60, 10, 60], "walls": False, "ceiling": False, "floor_size": [3000, 0, 3000], "floor_material": "asphalt",
             "floor_color": [0.32, 0.33, 0.34]},
    "spawns": [{"pos": points["spawn_bed"]["pos"], "yaw": 0}, {"pos": points["spawn_hall"]["pos"], "yaw": 0},
               {"pos": points["spawn_teen"]["pos"], "yaw": 90}, {"pos": [5.25, FLOOR, 13.2], "yaw": 0}],
    "brushes": brushes,
    "entities": entities,
    "story": {"_doc": "src/story/story_run.c: points (spawn / action targets), volumes (boxes; same name = union), "
                      "interactables (markers made usable: read / use / hold / pickup), power groups (light groups).",
              "points": points, "volumes": volumes, "interact": interact,
              "power": {"apt_10a": [10, 11, 12, 13, 14, 15], "building": [20, 21]}},
}
with open(OUT, "w", newline="") as f:
    json.dump(level, f, indent=1)
print("wrote %s: %d brushes, %d entities, %d interactables, %d volumes" % (OUT, len(brushes), len(entities), len(interact), len(volumes)))
